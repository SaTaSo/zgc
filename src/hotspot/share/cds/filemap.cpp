/*
 * Copyright (c) 2003, 2022, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "jvm.h"
#include "cds/archiveBuilder.hpp"
#include "cds/archiveHeapLoader.inline.hpp"
#include "cds/archiveUtils.inline.hpp"
#include "cds/cds_globals.hpp"
#include "cds/dynamicArchive.hpp"
#include "cds/filemap.hpp"
#include "cds/heapShared.hpp"
#include "cds/metaspaceShared.hpp"
#include "classfile/altHashing.hpp"
#include "classfile/classFileStream.hpp"
#include "classfile/classLoader.hpp"
#include "classfile/classLoader.inline.hpp"
#include "classfile/classLoaderData.inline.hpp"
#include "classfile/classLoaderExt.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "classfile/vmClasses.hpp"
#include "classfile/vmSymbols.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "logging/logMessage.hpp"
#include "memory/iterator.inline.hpp"
#include "memory/metadataFactory.hpp"
#include "memory/metaspaceClosure.hpp"
#include "memory/oopFactory.hpp"
#include "memory/universe.hpp"
#include "oops/compressedOops.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/objArrayOop.hpp"
#include "oops/oop.inline.hpp"
#include "prims/jvmtiExport.hpp"
#include "runtime/arguments.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/java.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.hpp"
#include "runtime/vm_version.hpp"
#include "services/memTracker.hpp"
#include "utilities/align.hpp"
#include "utilities/bitMap.inline.hpp"
#include "utilities/classpathStream.hpp"
#include "utilities/defaultStream.hpp"
#include "utilities/ostream.hpp"
#if INCLUDE_G1GC
#include "gc/g1/g1CollectedHeap.hpp"
#include "gc/g1/heapRegion.hpp"
#endif

# include <sys/stat.h>
# include <errno.h>

#ifndef O_BINARY       // if defined (Win32) use binary files.
#define O_BINARY 0     // otherwise do nothing.
#endif

// Complain and stop. All error conditions occurring during the writing of
// an archive file should stop the process.  Unrecoverable errors during
// the reading of the archive file should stop the process.

static void fail_exit(const char *msg, va_list ap) {
  // This occurs very early during initialization: tty is not initialized.
  jio_fprintf(defaultStream::error_stream(),
              "An error has occurred while processing the"
              " shared archive file.\n");
  jio_vfprintf(defaultStream::error_stream(), msg, ap);
  jio_fprintf(defaultStream::error_stream(), "\n");
  // Do not change the text of the below message because some tests check for it.
  vm_exit_during_initialization("Unable to use shared archive.", NULL);
}


void FileMapInfo::fail_stop(const char *msg, ...) {
        va_list ap;
  va_start(ap, msg);
  fail_exit(msg, ap);   // Never returns.
  va_end(ap);           // for completeness.
}


// Complain and continue.  Recoverable errors during the reading of the
// archive file may continue (with sharing disabled).
//
// If we continue, then disable shared spaces and close the file.

void FileMapInfo::fail_continue(const char *msg, ...) {
  va_list ap;
  va_start(ap, msg);
  if (PrintSharedArchiveAndExit && _validating_shared_path_table) {
    // If we are doing PrintSharedArchiveAndExit and some of the classpath entries
    // do not validate, we can still continue "limping" to validate the remaining
    // entries. No need to quit.
    tty->print("[");
    tty->vprint(msg, ap);
    tty->print_cr("]");
  } else {
    if (RequireSharedSpaces) {
      fail_exit(msg, ap);
    } else {
      if (log_is_enabled(Info, cds)) {
        LogStream ls(Log(cds)::info());
        ls.print("UseSharedSpaces: ");
        ls.vprint_cr(msg, ap);
      }
    }
  }
  va_end(ap);
}

// Fill in the fileMapInfo structure with data about this VM instance.

// This method copies the vm version info into header_version.  If the version is too
// long then a truncated version, which has a hash code appended to it, is copied.
//
// Using a template enables this method to verify that header_version is an array of
// length JVM_IDENT_MAX.  This ensures that the code that writes to the CDS file and
// the code that reads the CDS file will both use the same size buffer.  Hence, will
// use identical truncation.  This is necessary for matching of truncated versions.
template <int N> static void get_header_version(char (&header_version) [N]) {
  assert(N == JVM_IDENT_MAX, "Bad header_version size");

  const char *vm_version = VM_Version::internal_vm_info_string();
  const int version_len = (int)strlen(vm_version);

  memset(header_version, 0, JVM_IDENT_MAX);

  if (version_len < (JVM_IDENT_MAX-1)) {
    strcpy(header_version, vm_version);

  } else {
    // Get the hash value.  Use a static seed because the hash needs to return the same
    // value over multiple jvm invocations.
    uint32_t hash = AltHashing::halfsiphash_32(8191, (const uint8_t*)vm_version, version_len);

    // Truncate the ident, saving room for the 8 hex character hash value.
    strncpy(header_version, vm_version, JVM_IDENT_MAX-9);

    // Append the hash code as eight hex digits.
    sprintf(&header_version[JVM_IDENT_MAX-9], "%08x", hash);
    header_version[JVM_IDENT_MAX-1] = 0;  // Null terminate.
  }

  assert(header_version[JVM_IDENT_MAX-1] == 0, "must be");
}

FileMapInfo::FileMapInfo(const char* full_path, bool is_static) :
  _is_static(is_static), _file_open(false), _is_mapped(false), _fd(-1), _file_offset(0),
  _full_path(full_path), _base_archive_name(nullptr), _header(nullptr) {
  if (_is_static) {
    assert(_current_info == NULL, "must be singleton"); // not thread safe
    _current_info = this;
  } else {
    assert(_dynamic_archive_info == NULL, "must be singleton"); // not thread safe
    _dynamic_archive_info = this;
  }
}

FileMapInfo::~FileMapInfo() {
  if (_is_static) {
    assert(_current_info == this, "must be singleton"); // not thread safe
    _current_info = NULL;
  } else {
    assert(_dynamic_archive_info == this, "must be singleton"); // not thread safe
    _dynamic_archive_info = NULL;
  }

  if (_header != nullptr) {
    os::free(_header);
  }

  if (_file_open) {
    ::close(_fd);
  }
}

void FileMapInfo::populate_header(size_t core_region_alignment) {
  assert(_header == NULL, "Sanity check");
  size_t c_header_size;
  size_t header_size;
  size_t base_archive_name_size = 0;
  size_t base_archive_name_offset = 0;
  size_t longest_common_prefix_size = 0;
  if (is_static()) {
    c_header_size = sizeof(FileMapHeader);
    header_size = c_header_size;
  } else {
    // dynamic header including base archive name for non-default base archive
    c_header_size = sizeof(DynamicArchiveHeader);
    header_size = c_header_size;

    const char* default_base_archive_name = Arguments::get_default_shared_archive_path();
    const char* current_base_archive_name = Arguments::GetSharedArchivePath();
    if (!os::same_files(current_base_archive_name, default_base_archive_name)) {
      base_archive_name_size = strlen(current_base_archive_name) + 1;
      header_size += base_archive_name_size;
      base_archive_name_offset = c_header_size;
    }
    FREE_C_HEAP_ARRAY(const char, default_base_archive_name);
  }
  ResourceMark rm;
  GrowableArray<const char*>* app_cp_array = create_dumptime_app_classpath_array();
  int len = app_cp_array->length();
  longest_common_prefix_size = longest_common_app_classpath_prefix_len(len, app_cp_array);
  _header = (FileMapHeader*)os::malloc(header_size, mtInternal);
  memset((void*)_header, 0, header_size);
  _header->populate(this,
                    core_region_alignment,
                    header_size,
                    base_archive_name_size,
                    base_archive_name_offset,
                    longest_common_prefix_size);
}

void FileMapHeader::populate(FileMapInfo *info, size_t core_region_alignment,
                             size_t header_size, size_t base_archive_name_size,
                             size_t base_archive_name_offset, size_t common_app_classpath_prefix_size) {
  // 1. We require _generic_header._magic to be at the beginning of the file
  // 2. FileMapHeader also assumes that _generic_header is at the beginning of the file
  assert(offset_of(FileMapHeader, _generic_header) == 0, "must be");
  set_header_size((unsigned int)header_size);
  set_base_archive_name_offset((unsigned int)base_archive_name_offset);
  set_base_archive_name_size((unsigned int)base_archive_name_size);
  set_common_app_classpath_prefix_size((unsigned int)common_app_classpath_prefix_size);
  set_magic(DynamicDumpSharedSpaces ? CDS_DYNAMIC_ARCHIVE_MAGIC : CDS_ARCHIVE_MAGIC);
  set_version(CURRENT_CDS_ARCHIVE_VERSION);

  if (!info->is_static() && base_archive_name_size != 0) {
    // copy base archive name
    copy_base_archive_name(Arguments::GetSharedArchivePath());
  }
  _core_region_alignment = core_region_alignment;
  _obj_alignment = ObjectAlignmentInBytes;
  _compact_strings = CompactStrings;
  if (DumpSharedSpaces && HeapShared::can_write()) {
    _narrow_oop_mode = CompressedOops::mode();
    _narrow_oop_base = CompressedOops::base();
    _narrow_oop_shift = CompressedOops::shift();
    if (UseCompressedOops) {
      _heap_begin = CompressedOops::begin();
      _heap_end = CompressedOops::end();
    } else {
#if INCLUDE_G1GC
      address start = (address)G1CollectedHeap::heap()->reserved().start();
      address end = (address)G1CollectedHeap::heap()->reserved().end();
      _heap_begin = HeapShared::to_requested_address(start);
      _heap_end = HeapShared::to_requested_address(end);
#endif
    }
  }
  _compressed_oops = UseCompressedOops;
  _compressed_class_ptrs = UseCompressedClassPointers;
  _max_heap_size = MaxHeapSize;
  _narrow_klass_shift = CompressedKlassPointers::shift();
  _use_optimized_module_handling = MetaspaceShared::use_optimized_module_handling();
  _use_full_module_graph = MetaspaceShared::use_full_module_graph();

  // The following fields are for sanity checks for whether this archive
  // will function correctly with this JVM and the bootclasspath it's
  // invoked with.

  // JVM version string ... changes on each build.
  get_header_version(_jvm_ident);

  _app_class_paths_start_index = ClassLoaderExt::app_class_paths_start_index();
  _app_module_paths_start_index = ClassLoaderExt::app_module_paths_start_index();
  _num_module_paths = ClassLoader::num_module_path_entries();
  _max_used_path_index = ClassLoaderExt::max_used_path_index();

  _verify_local = BytecodeVerificationLocal;
  _verify_remote = BytecodeVerificationRemote;
  _has_platform_or_app_classes = ClassLoaderExt::has_platform_or_app_classes();
  _has_non_jar_in_classpath = ClassLoaderExt::has_non_jar_in_classpath();
  _requested_base_address = (char*)SharedBaseAddress;
  _mapped_base_address = (char*)SharedBaseAddress;
  _allow_archiving_with_java_agent = AllowArchivingWithJavaAgent;

  if (!DynamicDumpSharedSpaces) {
    set_shared_path_table(info->_shared_path_table);
  }
}

void FileMapHeader::copy_base_archive_name(const char* archive) {
  assert(base_archive_name_size() != 0, "_base_archive_name_size not set");
  assert(base_archive_name_offset() != 0, "_base_archive_name_offset not set");
  assert(header_size() > sizeof(*this), "_base_archive_name_size not included in header size?");
  memcpy((char*)this + base_archive_name_offset(), archive, base_archive_name_size());
}

void FileMapHeader::print(outputStream* st) {
  ResourceMark rm;

  st->print_cr("- magic:                          0x%08x", magic());
  st->print_cr("- crc:                            0x%08x", crc());
  st->print_cr("- version:                        0x%x", version());
  st->print_cr("- header_size:                    " UINT32_FORMAT, header_size());
  st->print_cr("- common_app_classpath_size:      " UINT32_FORMAT, common_app_classpath_prefix_size());
  st->print_cr("- base_archive_name_offset:       " UINT32_FORMAT, base_archive_name_offset());
  st->print_cr("- base_archive_name_size:         " UINT32_FORMAT, base_archive_name_size());

  for (int i = 0; i < NUM_CDS_REGIONS; i++) {
    FileMapRegion* si = space_at(i);
    si->print(st, i);
  }
  st->print_cr("============ end regions ======== ");

  st->print_cr("- core_region_alignment:          " SIZE_FORMAT, _core_region_alignment);
  st->print_cr("- obj_alignment:                  %d", _obj_alignment);
  st->print_cr("- narrow_oop_base:                " INTPTR_FORMAT, p2i(_narrow_oop_base));
  st->print_cr("- narrow_oop_base:                " INTPTR_FORMAT, p2i(_narrow_oop_base));
  st->print_cr("- narrow_oop_shift                %d", _narrow_oop_shift);
  st->print_cr("- compact_strings:                %d", _compact_strings);
  st->print_cr("- max_heap_size:                  " UINTX_FORMAT, _max_heap_size);
  st->print_cr("- narrow_oop_mode:                %d", _narrow_oop_mode);
  st->print_cr("- narrow_klass_shift:             %d", _narrow_klass_shift);
  st->print_cr("- compressed_oops:                %d", _compressed_oops);
  st->print_cr("- compressed_class_ptrs:          %d", _compressed_class_ptrs);
  st->print_cr("- cloned_vtables_offset:          " SIZE_FORMAT_X, _cloned_vtables_offset);
  st->print_cr("- serialized_data_offset:         " SIZE_FORMAT_X, _serialized_data_offset);
  st->print_cr("- heap_begin:                     " INTPTR_FORMAT, p2i(_heap_begin));
  st->print_cr("- heap_end:                       " INTPTR_FORMAT, p2i(_heap_end));
  st->print_cr("- jvm_ident:                      %s", _jvm_ident);
  st->print_cr("- shared_path_table_offset:       " SIZE_FORMAT_X, _shared_path_table_offset);
  st->print_cr("- shared_path_table_size:         %d", _shared_path_table_size);
  st->print_cr("- app_class_paths_start_index:    %d", _app_class_paths_start_index);
  st->print_cr("- app_module_paths_start_index:   %d", _app_module_paths_start_index);
  st->print_cr("- num_module_paths:               %d", _num_module_paths);
  st->print_cr("- max_used_path_index:            %d", _max_used_path_index);
  st->print_cr("- verify_local:                   %d", _verify_local);
  st->print_cr("- verify_remote:                  %d", _verify_remote);
  st->print_cr("- has_platform_or_app_classes:    %d", _has_platform_or_app_classes);
  st->print_cr("- has_non_jar_in_classpath:       %d", _has_non_jar_in_classpath);
  st->print_cr("- requested_base_address:         " INTPTR_FORMAT, p2i(_requested_base_address));
  st->print_cr("- mapped_base_address:            " INTPTR_FORMAT, p2i(_mapped_base_address));
  st->print_cr("- allow_archiving_with_java_agent:%d", _allow_archiving_with_java_agent);
  st->print_cr("- use_optimized_module_handling:  %d", _use_optimized_module_handling);
  st->print_cr("- use_full_module_graph           %d", _use_full_module_graph);
  st->print_cr("- ptrmap_size_in_bits:            " SIZE_FORMAT, _ptrmap_size_in_bits);
}

void SharedClassPathEntry::init_as_non_existent(const char* path, TRAPS) {
  _type = non_existent_entry;
  set_name(path, CHECK);
}

void SharedClassPathEntry::init(bool is_modules_image,
                                bool is_module_path,
                                ClassPathEntry* cpe, TRAPS) {
  Arguments::assert_is_dumping_archive();
  _timestamp = 0;
  _filesize  = 0;
  _from_class_path_attr = false;

  struct stat st;
  if (os::stat(cpe->name(), &st) == 0) {
    if ((st.st_mode & S_IFMT) == S_IFDIR) {
      _type = dir_entry;
    } else {
      // The timestamp of the modules_image is not checked at runtime.
      if (is_modules_image) {
        _type = modules_image_entry;
      } else {
        _type = jar_entry;
        _timestamp = st.st_mtime;
        _from_class_path_attr = cpe->from_class_path_attr();
      }
      _filesize = st.st_size;
      _is_module_path = is_module_path;
    }
  } else {
    // The file/dir must exist, or it would not have been added
    // into ClassLoader::classpath_entry().
    //
    // If we can't access a jar file in the boot path, then we can't
    // make assumptions about where classes get loaded from.
    FileMapInfo::fail_stop("Unable to open file %s.", cpe->name());
  }

  // No need to save the name of the module file, as it will be computed at run time
  // to allow relocation of the JDK directory.
  const char* name = is_modules_image  ? "" : cpe->name();
  set_name(name, CHECK);
}

void SharedClassPathEntry::set_name(const char* name, TRAPS) {
  size_t len = strlen(name) + 1;
  _name = MetadataFactory::new_array<char>(ClassLoaderData::the_null_class_loader_data(), (int)len, CHECK);
  strcpy(_name->data(), name);
}

void SharedClassPathEntry::copy_from(SharedClassPathEntry* ent, ClassLoaderData* loader_data, TRAPS) {
  _type = ent->_type;
  _is_module_path = ent->_is_module_path;
  _timestamp = ent->_timestamp;
  _filesize = ent->_filesize;
  _from_class_path_attr = ent->_from_class_path_attr;
  set_name(ent->name(), CHECK);

  if (ent->is_jar() && !ent->is_signed() && ent->manifest() != NULL) {
    Array<u1>* buf = MetadataFactory::new_array<u1>(loader_data,
                                                    ent->manifest_size(),
                                                    CHECK);
    char* p = (char*)(buf->data());
    memcpy(p, ent->manifest(), ent->manifest_size());
    set_manifest(buf);
  }
}

const char* SharedClassPathEntry::name() const {
  if (UseSharedSpaces && is_modules_image()) {
    // In order to validate the runtime modules image file size against the archived
    // size information, we need to obtain the runtime modules image path. The recorded
    // dump time modules image path in the archive may be different from the runtime path
    // if the JDK image has beed moved after generating the archive.
    return ClassLoader::get_jrt_entry()->name();
  } else {
    return _name->data();
  }
}

bool SharedClassPathEntry::validate(bool is_class_path) const {
  assert(UseSharedSpaces, "runtime only");

  struct stat st;
  const char* name = this->name();

  bool ok = true;
  log_info(class, path)("checking shared classpath entry: %s", name);
  if (os::stat(name, &st) != 0 && is_class_path) {
    // If the archived module path entry does not exist at runtime, it is not fatal
    // (no need to invalid the shared archive) because the shared runtime visibility check
    // filters out any archived module classes that do not have a matching runtime
    // module path location.
    FileMapInfo::fail_continue("Required classpath entry does not exist: %s", name);
    ok = false;
  } else if (is_dir()) {
    if (!os::dir_is_empty(name)) {
      FileMapInfo::fail_continue("directory is not empty: %s", name);
      ok = false;
    }
  } else if ((has_timestamp() && _timestamp != st.st_mtime) ||
             _filesize != st.st_size) {
    ok = false;
    if (PrintSharedArchiveAndExit) {
      FileMapInfo::fail_continue(_timestamp != st.st_mtime ?
                                 "Timestamp mismatch" :
                                 "File size mismatch");
    } else {
      const char* bad_jar_msg = "A jar file is not the one used while building the shared archive file:";
      FileMapInfo::fail_continue("%s %s", bad_jar_msg, name);
      if (!log_is_enabled(Info, cds)) {
        log_warning(cds)("%s %s", bad_jar_msg, name);
      }
      if (_timestamp != st.st_mtime) {
        log_warning(cds)("%s timestamp has changed.", name);
      } else {
        log_warning(cds)("%s size has changed.", name);
      }
    }
  }

  if (PrintSharedArchiveAndExit && !ok) {
    // If PrintSharedArchiveAndExit is enabled, don't report failure to the
    // caller. Please see above comments for more details.
    ok = true;
    MetaspaceShared::set_archive_loading_failed();
  }
  return ok;
}

bool SharedClassPathEntry::check_non_existent() const {
  assert(_type == non_existent_entry, "must be");
  log_info(class, path)("should be non-existent: %s", name());
  struct stat st;
  if (os::stat(name(), &st) != 0) {
    log_info(class, path)("ok");
    return true; // file doesn't exist
  } else {
    return false;
  }
}


void SharedClassPathEntry::metaspace_pointers_do(MetaspaceClosure* it) {
  it->push(&_name);
  it->push(&_manifest);
}

void SharedPathTable::metaspace_pointers_do(MetaspaceClosure* it) {
  it->push(&_table);
  for (int i=0; i<_size; i++) {
    path_at(i)->metaspace_pointers_do(it);
  }
}

void SharedPathTable::dumptime_init(ClassLoaderData* loader_data, TRAPS) {
  size_t entry_size = sizeof(SharedClassPathEntry);
  int num_entries = 0;
  num_entries += ClassLoader::num_boot_classpath_entries();
  num_entries += ClassLoader::num_app_classpath_entries();
  num_entries += ClassLoader::num_module_path_entries();
  num_entries += FileMapInfo::num_non_existent_class_paths();
  size_t bytes = entry_size * num_entries;

  _table = MetadataFactory::new_array<u8>(loader_data, (int)bytes, CHECK);
  _size = num_entries;
}

// Make a copy of the _shared_path_table for use during dynamic CDS dump.
// It is needed because some Java code continues to execute after dynamic dump has finished.
// However, during dynamic dump, we have modified FileMapInfo::_shared_path_table so
// FileMapInfo::shared_path(i) returns incorrect information in ClassLoader::record_result().
void FileMapInfo::copy_shared_path_table(ClassLoaderData* loader_data, TRAPS) {
  size_t entry_size = sizeof(SharedClassPathEntry);
  size_t bytes = entry_size * _shared_path_table.size();

  Array<u8>* array = MetadataFactory::new_array<u8>(loader_data, (int)bytes, CHECK);
  _saved_shared_path_table = SharedPathTable(array, _shared_path_table.size());

  for (int i = 0; i < _shared_path_table.size(); i++) {
    _saved_shared_path_table.path_at(i)->copy_from(shared_path(i), loader_data, CHECK);
  }
  _saved_shared_path_table_array = array;
}

void FileMapInfo::clone_shared_path_table(TRAPS) {
  Arguments::assert_is_dumping_archive();

  ClassLoaderData* loader_data = ClassLoaderData::the_null_class_loader_data();
  ClassPathEntry* jrt = ClassLoader::get_jrt_entry();

  assert(jrt != NULL,
         "No modular java runtime image present when allocating the CDS classpath entry table");

  if (_saved_shared_path_table_array != NULL) {
    MetadataFactory::free_array<u8>(loader_data, _saved_shared_path_table_array);
    _saved_shared_path_table_array = NULL;
  }

  copy_shared_path_table(loader_data, CHECK);
}

void FileMapInfo::allocate_shared_path_table(TRAPS) {
  Arguments::assert_is_dumping_archive();

  ClassLoaderData* loader_data = ClassLoaderData::the_null_class_loader_data();
  ClassPathEntry* jrt = ClassLoader::get_jrt_entry();

  assert(jrt != NULL,
         "No modular java runtime image present when allocating the CDS classpath entry table");

  _shared_path_table.dumptime_init(loader_data, CHECK);

  // 1. boot class path
  int i = 0;
  i = add_shared_classpaths(i, "boot",   jrt, CHECK);
  i = add_shared_classpaths(i, "app",    ClassLoader::app_classpath_entries(), CHECK);
  i = add_shared_classpaths(i, "module", ClassLoader::module_path_entries(), CHECK);

  for (int x = 0; x < num_non_existent_class_paths(); x++, i++) {
    const char* path = _non_existent_class_paths->at(x);
    shared_path(i)->init_as_non_existent(path, CHECK);
  }

  assert(i == _shared_path_table.size(), "number of shared path entry mismatch");
  clone_shared_path_table(CHECK);
}

int FileMapInfo::add_shared_classpaths(int i, const char* which, ClassPathEntry *cpe, TRAPS) {
  while (cpe != NULL) {
    bool is_jrt = (cpe == ClassLoader::get_jrt_entry());
    bool is_module_path = i >= ClassLoaderExt::app_module_paths_start_index();
    const char* type = (is_jrt ? "jrt" : (cpe->is_jar_file() ? "jar" : "dir"));
    log_info(class, path)("add %s shared path (%s) %s", which, type, cpe->name());
    SharedClassPathEntry* ent = shared_path(i);
    ent->init(is_jrt, is_module_path, cpe, CHECK_0);
    if (cpe->is_jar_file()) {
      update_jar_manifest(cpe, ent, CHECK_0);
    }
    if (is_jrt) {
      cpe = ClassLoader::get_next_boot_classpath_entry(cpe);
    } else {
      cpe = cpe->next();
    }
    i++;
  }

  return i;
}

void FileMapInfo::check_nonempty_dir_in_shared_path_table() {
  Arguments::assert_is_dumping_archive();

  bool has_nonempty_dir = false;

  int last = _shared_path_table.size() - 1;
  if (last > ClassLoaderExt::max_used_path_index()) {
     // no need to check any path beyond max_used_path_index
     last = ClassLoaderExt::max_used_path_index();
  }

  for (int i = 0; i <= last; i++) {
    SharedClassPathEntry *e = shared_path(i);
    if (e->is_dir()) {
      const char* path = e->name();
      if (!os::dir_is_empty(path)) {
        log_error(cds)("Error: non-empty directory '%s'", path);
        has_nonempty_dir = true;
      }
    }
  }

  if (has_nonempty_dir) {
    ClassLoader::exit_with_path_failure("Cannot have non-empty directory in paths", NULL);
  }
}

void FileMapInfo::record_non_existent_class_path_entry(const char* path) {
  Arguments::assert_is_dumping_archive();
  log_info(class, path)("non-existent Class-Path entry %s", path);
  if (_non_existent_class_paths == NULL) {
    _non_existent_class_paths = new (ResourceObj::C_HEAP, mtClass)GrowableArray<const char*>(10, mtClass);
  }
  _non_existent_class_paths->append(os::strdup(path));
}

int FileMapInfo::num_non_existent_class_paths() {
  Arguments::assert_is_dumping_archive();
  if (_non_existent_class_paths != NULL) {
    return _non_existent_class_paths->length();
  } else {
    return 0;
  }
}

int FileMapInfo::get_module_shared_path_index(Symbol* location) {
  if (location->starts_with("jrt:", 4) && get_number_of_shared_paths() > 0) {
    assert(shared_path(0)->is_modules_image(), "first shared_path must be the modules image");
    return 0;
  }

  if (ClassLoaderExt::app_module_paths_start_index() >= get_number_of_shared_paths()) {
    // The archive(s) were created without --module-path option
    return -1;
  }

  if (!location->starts_with("file:", 5)) {
    return -1;
  }

  // skip_uri_protocol was also called during dump time -- see ClassLoaderExt::process_module_table()
  ResourceMark rm;
  const char* file = ClassLoader::skip_uri_protocol(location->as_C_string());
  for (int i = ClassLoaderExt::app_module_paths_start_index(); i < get_number_of_shared_paths(); i++) {
    SharedClassPathEntry* ent = shared_path(i);
    assert(ent->in_named_module(), "must be");
    bool cond = strcmp(file, ent->name()) == 0;
    log_debug(class, path)("get_module_shared_path_index (%d) %s : %s = %s", i,
                           location->as_C_string(), ent->name(), cond ? "same" : "different");
    if (cond) {
      return i;
    }
  }

  return -1;
}

class ManifestStream: public ResourceObj {
  private:
  u1*   _buffer_start; // Buffer bottom
  u1*   _buffer_end;   // Buffer top (one past last element)
  u1*   _current;      // Current buffer position

 public:
  // Constructor
  ManifestStream(u1* buffer, int length) : _buffer_start(buffer),
                                           _current(buffer) {
    _buffer_end = buffer + length;
  }

  static bool is_attr(u1* attr, const char* name) {
    return strncmp((const char*)attr, name, strlen(name)) == 0;
  }

  static char* copy_attr(u1* value, size_t len) {
    char* buf = NEW_RESOURCE_ARRAY(char, len + 1);
    strncpy(buf, (char*)value, len);
    buf[len] = 0;
    return buf;
  }

  // The return value indicates if the JAR is signed or not
  bool check_is_signed() {
    u1* attr = _current;
    bool isSigned = false;
    while (_current < _buffer_end) {
      if (*_current == '\n') {
        *_current = '\0';
        u1* value = (u1*)strchr((char*)attr, ':');
        if (value != NULL) {
          assert(*(value+1) == ' ', "Unrecognized format" );
          if (strstr((char*)attr, "-Digest") != NULL) {
            isSigned = true;
            break;
          }
        }
        *_current = '\n'; // restore
        attr = _current + 1;
      }
      _current ++;
    }
    return isSigned;
  }
};

void FileMapInfo::update_jar_manifest(ClassPathEntry *cpe, SharedClassPathEntry* ent, TRAPS) {
  ClassLoaderData* loader_data = ClassLoaderData::the_null_class_loader_data();
  ResourceMark rm(THREAD);
  jint manifest_size;

  assert(cpe->is_jar_file() && ent->is_jar(), "the shared class path entry is not a JAR file");
  char* manifest = ClassLoaderExt::read_manifest(THREAD, cpe, &manifest_size);
  if (manifest != NULL) {
    ManifestStream* stream = new ManifestStream((u1*)manifest,
                                                manifest_size);
    if (stream->check_is_signed()) {
      ent->set_is_signed();
    } else {
      // Copy the manifest into the shared archive
      manifest = ClassLoaderExt::read_raw_manifest(THREAD, cpe, &manifest_size);
      Array<u1>* buf = MetadataFactory::new_array<u1>(loader_data,
                                                      manifest_size,
                                                      CHECK);
      char* p = (char*)(buf->data());
      memcpy(p, manifest, manifest_size);
      ent->set_manifest(buf);
    }
  }
}

char* FileMapInfo::skip_first_path_entry(const char* path) {
  size_t path_sep_len = strlen(os::path_separator());
  char* p = strstr((char*)path, os::path_separator());
  if (p != NULL) {
    debug_only( {
      size_t image_name_len = strlen(MODULES_IMAGE_NAME);
      assert(strncmp(p - image_name_len, MODULES_IMAGE_NAME, image_name_len) == 0,
             "first entry must be the modules image");
    } );
    p += path_sep_len;
  } else {
    debug_only( {
      assert(ClassLoader::string_ends_with(path, MODULES_IMAGE_NAME),
             "first entry must be the modules image");
    } );
  }
  return p;
}

int FileMapInfo::num_paths(const char* path) {
  if (path == NULL) {
    return 0;
  }
  int npaths = 1;
  char* p = (char*)path;
  while (p != NULL) {
    char* prev = p;
    p = strstr((char*)p, os::path_separator());
    if (p != NULL) {
      p++;
      // don't count empty path
      if ((p - prev) > 1) {
       npaths++;
      }
    }
  }
  return npaths;
}

// Returns true if a path within the paths exists and has non-zero size.
bool FileMapInfo::check_paths_existence(const char* paths) {
  ClasspathStream cp_stream(paths);
  bool exist = false;
  struct stat st;
  while (cp_stream.has_next()) {
    const char* path = cp_stream.get_next();
    if (os::stat(path, &st) == 0 && st.st_size > 0) {
      exist = true;
      break;
    }
  }
  return exist;
}

GrowableArray<const char*>* FileMapInfo::create_dumptime_app_classpath_array() {
  Arguments::assert_is_dumping_archive();
  GrowableArray<const char*>* path_array = new GrowableArray<const char*>(10);
  ClassPathEntry* cpe = ClassLoader::app_classpath_entries();
  while (cpe != NULL) {
    path_array->append(cpe->name());
    cpe = cpe->next();
  }
  return path_array;
}

GrowableArray<const char*>* FileMapInfo::create_path_array(const char* paths) {
  GrowableArray<const char*>* path_array = new GrowableArray<const char*>(10);
  JavaThread* current = JavaThread::current();
  ClasspathStream cp_stream(paths);
  bool non_jar_in_cp = header()->has_non_jar_in_classpath();
  while (cp_stream.has_next()) {
    const char* path = cp_stream.get_next();
    if (!non_jar_in_cp) {
      struct stat st;
      if (os::stat(path, &st) == 0) {
        path_array->append(path);
      }
    } else {
      const char* canonical_path = ClassLoader::get_canonical_path(path, current);
      if (canonical_path != NULL) {
        char* error_msg = NULL;
        jzfile* zip = ClassLoader::open_zip_file(canonical_path, &error_msg, current);
        if (zip != NULL && error_msg == NULL) {
          path_array->append(path);
        }
      }
    }
  }
  return path_array;
}

bool FileMapInfo::classpath_failure(const char* msg, const char* name) {
  ClassLoader::trace_class_path(msg, name);
  if (PrintSharedArchiveAndExit) {
    MetaspaceShared::set_archive_loading_failed();
  }
  return false;
}

unsigned int FileMapInfo::longest_common_app_classpath_prefix_len(int num_paths,
                                                                  GrowableArray<const char*>* rp_array) {
  if (num_paths == 0) {
    return 0;
  }
  unsigned int pos;
  for (pos = 0; ; pos++) {
    for (int i = 0; i < num_paths; i++) {
      if (rp_array->at(i)[pos] != '\0' && rp_array->at(i)[pos] == rp_array->at(0)[pos]) {
        continue;
      }

      // search backward for the pos before the file separator char
      while (pos > 0 && rp_array->at(0)[--pos] != *os::file_separator());
      // return the file separator char position
      return pos + 1;
    }
  }
  return 0;
}

bool FileMapInfo::check_paths(int shared_path_start_idx, int num_paths, GrowableArray<const char*>* rp_array,
                              unsigned int dumptime_prefix_len, unsigned int runtime_prefix_len) {
  int i = 0;
  int j = shared_path_start_idx;
  while (i < num_paths) {
    while (shared_path(j)->from_class_path_attr()) {
      // shared_path(j) was expanded from the JAR file attribute "Class-Path:"
      // during dump time. It's not included in the -classpath VM argument.
      j++;
    }
    assert(strlen(shared_path(j)->name()) > (size_t)dumptime_prefix_len, "sanity");
    const char* dumptime_path = shared_path(j)->name() + dumptime_prefix_len;
    assert(strlen(rp_array->at(i)) > (size_t)runtime_prefix_len, "sanity");
    const char* runtime_path = rp_array->at(i)  + runtime_prefix_len;
    if (!os::same_files(dumptime_path, runtime_path)) {
      return true;
    }
    i++;
    j++;
  }
  return false;
}

bool FileMapInfo::validate_boot_class_paths() {
  //
  // - Archive contains boot classes only - relaxed boot path check:
  //   Extra path elements appended to the boot path at runtime are allowed.
  //
  // - Archive contains application or platform classes - strict boot path check:
  //   Validate the entire runtime boot path, which must be compatible
  //   with the dump time boot path. Appending boot path at runtime is not
  //   allowed.
  //

  // The first entry in boot path is the modules_image (guaranteed by
  // ClassLoader::setup_boot_search_path()). Skip the first entry. The
  // path of the runtime modules_image may be different from the dump
  // time path (e.g. the JDK image is copied to a different location
  // after generating the shared archive), which is acceptable. For most
  // common cases, the dump time boot path might contain modules_image only.
  char* runtime_boot_path = Arguments::get_boot_class_path();
  char* rp = skip_first_path_entry(runtime_boot_path);
  assert(shared_path(0)->is_modules_image(), "first shared_path must be the modules image");
  int dp_len = header()->app_class_paths_start_index() - 1; // ignore the first path to the module image
  bool mismatch = false;

  bool relaxed_check = !header()->has_platform_or_app_classes();
  if (dp_len == 0 && rp == NULL) {
    return true;   // ok, both runtime and dump time boot paths have modules_images only
  } else if (dp_len == 0 && rp != NULL) {
    if (relaxed_check) {
      return true;   // ok, relaxed check, runtime has extra boot append path entries
    } else {
      ResourceMark rm;
      if (check_paths_existence(rp)) {
        // If a path exists in the runtime boot paths, it is considered a mismatch
        // since there's no boot path specified during dump time.
        mismatch = true;
      }
    }
  } else if (dp_len > 0 && rp != NULL) {
    int num;
    ResourceMark rm;
    GrowableArray<const char*>* rp_array = create_path_array(rp);
    int rp_len = rp_array->length();
    if (rp_len >= dp_len) {
      if (relaxed_check) {
        // only check the leading entries in the runtime boot path, up to
        // the length of the dump time boot path
        num = dp_len;
      } else {
        // check the full runtime boot path, must match with dump time
        num = rp_len;
      }
      mismatch = check_paths(1, num, rp_array, 0, 0);
    } else {
      // create_path_array() ignores non-existing paths. Although the dump time and runtime boot classpath lengths
      // are the same initially, after the call to create_path_array(), the runtime boot classpath length could become
      // shorter. We consider boot classpath mismatch in this case.
      mismatch = true;
    }
  }

  if (mismatch) {
    // The paths are different
    return classpath_failure("[BOOT classpath mismatch, actual =", runtime_boot_path);
  }
  return true;
}

bool FileMapInfo::validate_app_class_paths(int shared_app_paths_len) {
  const char *appcp = Arguments::get_appclasspath();
  assert(appcp != NULL, "NULL app classpath");
  int rp_len = num_paths(appcp);
  bool mismatch = false;
  if (rp_len < shared_app_paths_len) {
    return classpath_failure("Run time APP classpath is shorter than the one at dump time: ", appcp);
  }
  if (shared_app_paths_len != 0 && rp_len != 0) {
    // Prefix is OK: E.g., dump with -cp foo.jar, but run with -cp foo.jar:bar.jar.
    ResourceMark rm;
    GrowableArray<const char*>* rp_array = create_path_array(appcp);
    if (rp_array->length() == 0) {
      // None of the jar file specified in the runtime -cp exists.
      return classpath_failure("None of the jar file specified in the runtime -cp exists: -Djava.class.path=", appcp);
    }
    if (rp_array->length() < shared_app_paths_len) {
      // create_path_array() ignores non-existing paths. Although the dump time and runtime app classpath lengths
      // are the same initially, after the call to create_path_array(), the runtime app classpath length could become
      // shorter. We consider app classpath mismatch in this case.
      return classpath_failure("[APP classpath mismatch, actual: -Djava.class.path=", appcp);
    }

    // Handling of non-existent entries in the classpath: we eliminate all the non-existent
    // entries from both the dump time classpath (ClassLoader::update_class_path_entry_list)
    // and the runtime classpath (FileMapInfo::create_path_array), and check the remaining
    // entries. E.g.:
    //
    // dump : -cp a.jar:NE1:NE2:b.jar  -> a.jar:b.jar -> recorded in archive.
    // run 1: -cp NE3:a.jar:NE4:b.jar  -> a.jar:b.jar -> matched
    // run 2: -cp x.jar:NE4:b.jar      -> x.jar:b.jar -> mismatched

    int j = header()->app_class_paths_start_index();
    mismatch = check_paths(j, shared_app_paths_len, rp_array, 0, 0);
    if (mismatch) {
      // To facilitate app deployment, we allow the JAR files to be moved *together* to
      // a different location, as long as they are still stored under the same directory
      // structure. E.g., the following is OK.
      //     java -Xshare:dump -cp /a/Foo.jar:/a/b/Bar.jar  ...
      //     java -Xshare:auto -cp /x/y/Foo.jar:/x/y/b/Bar.jar  ...
      unsigned int dumptime_prefix_len = header()->common_app_classpath_prefix_size();
      unsigned int runtime_prefix_len = longest_common_app_classpath_prefix_len(shared_app_paths_len, rp_array);
      mismatch = check_paths(j, shared_app_paths_len, rp_array,
                             dumptime_prefix_len, runtime_prefix_len);
      if (mismatch) {
        return classpath_failure("[APP classpath mismatch, actual: -Djava.class.path=", appcp);
      }
    }
  }
  return true;
}

void FileMapInfo::log_paths(const char* msg, int start_idx, int end_idx) {
  LogTarget(Info, class, path) lt;
  if (lt.is_enabled()) {
    LogStream ls(lt);
    ls.print("%s", msg);
    const char* prefix = "";
    for (int i = start_idx; i < end_idx; i++) {
      ls.print("%s%s", prefix, shared_path(i)->name());
      prefix = os::path_separator();
    }
    ls.cr();
  }
}

bool FileMapInfo::check_module_paths() {
  const char* rp = Arguments::get_property("jdk.module.path");
  int num_paths = Arguments::num_archives(rp);
  if (num_paths != header()->num_module_paths()) {
    return false;
  }
  ResourceMark rm;
  GrowableArray<const char*>* rp_array = create_path_array(rp);
  return check_paths(header()->app_module_paths_start_index(), num_paths, rp_array, 0, 0);
}

bool FileMapInfo::validate_shared_path_table() {
  assert(UseSharedSpaces, "runtime only");

  _validating_shared_path_table = true;

  // Load the shared path table info from the archive header
  _shared_path_table = header()->shared_path_table();
  if (DynamicDumpSharedSpaces) {
    // Only support dynamic dumping with the usage of the default CDS archive
    // or a simple base archive.
    // If the base layer archive contains additional path component besides
    // the runtime image and the -cp, dynamic dumping is disabled.
    //
    // When dynamic archiving is enabled, the _shared_path_table is overwritten
    // to include the application path and stored in the top layer archive.
    assert(shared_path(0)->is_modules_image(), "first shared_path must be the modules image");
    if (header()->app_class_paths_start_index() > 1) {
      DynamicDumpSharedSpaces = false;
      warning(
        "Dynamic archiving is disabled because base layer archive has appended boot classpath");
    }
    if (header()->num_module_paths() > 0) {
      if (!check_module_paths()) {
        DynamicDumpSharedSpaces = false;
        warning(
          "Dynamic archiving is disabled because base layer archive has a different module path");
      }
    }
  }

  log_paths("Expecting BOOT path=", 0, header()->app_class_paths_start_index());
  log_paths("Expecting -Djava.class.path=", header()->app_class_paths_start_index(), header()->app_module_paths_start_index());

  int module_paths_start_index = header()->app_module_paths_start_index();
  int shared_app_paths_len = 0;

  // validate the path entries up to the _max_used_path_index
  for (int i=0; i < header()->max_used_path_index() + 1; i++) {
    if (i < module_paths_start_index) {
      if (shared_path(i)->validate()) {
        // Only count the app class paths not from the "Class-path" attribute of a jar manifest.
        if (!shared_path(i)->from_class_path_attr() && i >= header()->app_class_paths_start_index()) {
          shared_app_paths_len++;
        }
        log_info(class, path)("ok");
      } else {
        if (_dynamic_archive_info != NULL && _dynamic_archive_info->_is_static) {
          assert(!UseSharedSpaces, "UseSharedSpaces should be disabled");
        }
        return false;
      }
    } else if (i >= module_paths_start_index) {
      if (shared_path(i)->validate(false /* not a class path entry */)) {
        log_info(class, path)("ok");
      } else {
        if (_dynamic_archive_info != NULL && _dynamic_archive_info->_is_static) {
          assert(!UseSharedSpaces, "UseSharedSpaces should be disabled");
        }
        return false;
      }
    }
  }

  if (header()->max_used_path_index() == 0) {
    // default archive only contains the module image in the bootclasspath
    assert(shared_path(0)->is_modules_image(), "first shared_path must be the modules image");
  } else {
    if (!validate_boot_class_paths() || !validate_app_class_paths(shared_app_paths_len)) {
      const char* mismatch_msg = "shared class paths mismatch (hint: enable -Xlog:class+path=info to diagnose the failure)";
      fail_continue("%s", mismatch_msg);
      if (!log_is_enabled(Info, cds) && !log_is_enabled(Info, class, path)) {
        log_warning(cds)("%s", mismatch_msg);
      }
      return false;
    }
  }

  validate_non_existent_class_paths();

  _validating_shared_path_table = false;

#if INCLUDE_JVMTI
  if (_classpath_entries_for_jvmti != NULL) {
    os::free(_classpath_entries_for_jvmti);
  }
  size_t sz = sizeof(ClassPathEntry*) * get_number_of_shared_paths();
  _classpath_entries_for_jvmti = (ClassPathEntry**)os::malloc(sz, mtClass);
  memset((void*)_classpath_entries_for_jvmti, 0, sz);
#endif

  return true;
}

void FileMapInfo::validate_non_existent_class_paths() {
  // All of the recorded non-existent paths came from the Class-Path: attribute from the JAR
  // files on the app classpath. If any of these are found to exist during runtime,
  // it will change how classes are loading for the app loader. For safety, disable
  // loading of archived platform/app classes (currently there's no way to disable just the
  // app classes).

  assert(UseSharedSpaces, "runtime only");
  for (int i = header()->app_module_paths_start_index() + header()->num_module_paths();
       i < get_number_of_shared_paths();
       i++) {
    SharedClassPathEntry* ent = shared_path(i);
    if (!ent->check_non_existent()) {
      warning("Archived non-system classes are disabled because the "
              "file %s exists", ent->name());
      header()->set_has_platform_or_app_classes(false);
    }
  }
}

// A utility class for reading/validating the GenericCDSFileMapHeader portion of
// a CDS archive's header. The file header of all CDS archives with versions from
// CDS_GENERIC_HEADER_SUPPORTED_MIN_VERSION (12) are guaranteed to always start
// with GenericCDSFileMapHeader. This makes it possible to read important information
// from a CDS archive created by a different version of HotSpot, so that we can
// automatically regenerate the archive as necessary (JDK-8261455).
class FileHeaderHelper {
  int _fd;
  bool _is_valid;
  bool _is_static;
  GenericCDSFileMapHeader* _header;
  const char* _archive_name;
  const char* _base_archive_name;

public:
  FileHeaderHelper(const char* archive_name, bool is_static) {
    _fd = -1;
    _is_valid = false;
    _header = nullptr;
    _base_archive_name = nullptr;
    _archive_name = archive_name;
    _is_static = is_static;
  }

  ~FileHeaderHelper() {
    if (_header != nullptr) {
      FREE_C_HEAP_ARRAY(char, _header);
    }
    if (_fd != -1) {
      ::close(_fd);
    }
  }

  bool initialize() {
    assert(_archive_name != nullptr, "Archive name is NULL");
    _fd = os::open(_archive_name, O_RDONLY | O_BINARY, 0);
    if (_fd < 0) {
      FileMapInfo::fail_continue("Specified shared archive not found (%s)", _archive_name);
      return false;
    }
    return initialize(_fd);
  }

  // for an already opened file, do not set _fd
  bool initialize(int fd) {
    assert(_archive_name != nullptr, "Archive name is NULL");
    assert(fd != -1, "Archive must be opened already");
    // First read the generic header so we know the exact size of the actual header.
    GenericCDSFileMapHeader gen_header;
    size_t size = sizeof(GenericCDSFileMapHeader);
    os::lseek(fd, 0, SEEK_SET);
    size_t n = ::read(fd, (void*)&gen_header, (unsigned int)size);
    if (n != size) {
      FileMapInfo::fail_continue("Unable to read generic CDS file map header from shared archive");
      return false;
    }

    if (gen_header._magic != CDS_ARCHIVE_MAGIC &&
        gen_header._magic != CDS_DYNAMIC_ARCHIVE_MAGIC) {
      FileMapInfo::fail_continue("The shared archive file has a bad magic number: %#x", gen_header._magic);
      return false;
    }

    if (gen_header._version < CDS_GENERIC_HEADER_SUPPORTED_MIN_VERSION) {
      FileMapInfo::fail_continue("Cannot handle shared archive file version 0x%x. Must be at least 0x%x.",
                                 gen_header._version, CDS_GENERIC_HEADER_SUPPORTED_MIN_VERSION);
      return false;
    }

    if (gen_header._version !=  CURRENT_CDS_ARCHIVE_VERSION) {
      FileMapInfo::fail_continue("The shared archive file version 0x%x does not match the required version 0x%x.",
                                 gen_header._version, CURRENT_CDS_ARCHIVE_VERSION);
    }

    size_t filelen = os::lseek(fd, 0, SEEK_END);
    if (gen_header._header_size >= filelen) {
      FileMapInfo::fail_continue("Archive file header larger than archive file");
      return false;
    }

    // Read the actual header and perform more checks
    size = gen_header._header_size;
    _header = (GenericCDSFileMapHeader*)NEW_C_HEAP_ARRAY(char, size, mtInternal);
    os::lseek(fd, 0, SEEK_SET);
    n = ::read(fd, (void*)_header, (unsigned int)size);
    if (n != size) {
      FileMapInfo::fail_continue("Unable to read actual CDS file map header from shared archive");
      return false;
    }

    if (!check_crc()) {
      return false;
    }

    if (!check_and_init_base_archive_name()) {
      return false;
    }

    if (!check_common_app_classpath_prefix_len()) {
      return false;
    }

    // All fields in the GenericCDSFileMapHeader has been validated.
    _is_valid = true;
    return true;
  }

  GenericCDSFileMapHeader* get_generic_file_header() {
    assert(_header != nullptr && _is_valid, "must be a valid archive file");
    return _header;
  }

  const char* base_archive_name() {
    assert(_header != nullptr && _is_valid, "must be a valid archive file");
    return _base_archive_name;
  }

 private:
  bool check_crc() {
    if (VerifySharedSpaces) {
      FileMapHeader* header = (FileMapHeader*)_header;
      int actual_crc = header->compute_crc();
      if (actual_crc != header->crc()) {
        log_info(cds)("_crc expected: %d", header->crc());
        log_info(cds)("       actual: %d", actual_crc);
        FileMapInfo::fail_continue("Header checksum verification failed.");
        return false;
      }
    }
    return true;
  }

  bool check_and_init_base_archive_name() {
    unsigned int name_offset = _header->_base_archive_name_offset;
    unsigned int name_size   = _header->_base_archive_name_size;
    unsigned int header_size = _header->_header_size;

    if (name_offset + name_size < name_offset) {
      FileMapInfo::fail_continue("base_archive_name offset/size overflow: " UINT32_FORMAT "/" UINT32_FORMAT,
                                 name_offset, name_size);
      return false;
    }
    if (_header->_magic == CDS_ARCHIVE_MAGIC) {
      if (name_offset != 0) {
        FileMapInfo::fail_continue("static shared archive must have zero _base_archive_name_offset");
        return false;
      }
      if (name_size != 0) {
        FileMapInfo::fail_continue("static shared archive must have zero _base_archive_name_size");
        return false;
      }
    } else {
      assert(_header->_magic == CDS_DYNAMIC_ARCHIVE_MAGIC, "must be");
      if ((name_size == 0 && name_offset != 0) ||
          (name_size != 0 && name_offset == 0)) {
        // If either is zero, both must be zero. This indicates that we are using the default base archive.
        FileMapInfo::fail_continue("Invalid base_archive_name offset/size: " UINT32_FORMAT "/" UINT32_FORMAT,
                                   name_offset, name_size);
        return false;
      }
      if (name_size > 0) {
        if (name_offset + name_size > header_size) {
          FileMapInfo::fail_continue("Invalid base_archive_name offset/size (out of range): "
                                     UINT32_FORMAT " + " UINT32_FORMAT " > " UINT32_FORMAT ,
                                     name_offset, name_size, header_size);
          return false;
        }
        const char* name = ((const char*)_header) + _header->_base_archive_name_offset;
        if (name[name_size - 1] != '\0' || strlen(name) != name_size - 1) {
          FileMapInfo::fail_continue("Base archive name is damaged");
          return false;
        }
        if (!os::file_exists(name)) {
          FileMapInfo::fail_continue("Base archive %s does not exist", name);
          return false;
        }
        _base_archive_name = name;
      }
    }

    return true;
  }

  bool check_common_app_classpath_prefix_len() {
    int common_path_size = _header->_common_app_classpath_prefix_size;
    if (common_path_size < 0) {
      FileMapInfo::fail_continue("common app classpath prefix len < 0");
      return false;
    }
    return true;
  }
};

// Return value:
// false:
//      <archive_name> is not a valid archive. *base_archive_name is set to null.
// true && (*base_archive_name) == NULL:
//      <archive_name> is a valid static archive.
// true && (*base_archive_name) != NULL:
//      <archive_name> is a valid dynamic archive.
bool FileMapInfo::get_base_archive_name_from_header(const char* archive_name,
                                                    char** base_archive_name) {
  FileHeaderHelper file_helper(archive_name, false);
  *base_archive_name = NULL;

  if (!file_helper.initialize()) {
    return false;
  }
  GenericCDSFileMapHeader* header = file_helper.get_generic_file_header();
  if (header->_magic != CDS_DYNAMIC_ARCHIVE_MAGIC) {
    assert(header->_magic == CDS_ARCHIVE_MAGIC, "must be");
    if (AutoCreateSharedArchive) {
     log_warning(cds)("AutoCreateSharedArchive is ignored because %s is a static archive", archive_name);
    }
    return true;
  }

  const char* base = file_helper.base_archive_name();
  if (base == nullptr) {
    *base_archive_name = Arguments::get_default_shared_archive_path();
  } else {
    *base_archive_name = os::strdup_check_oom(base);
  }

  return true;
}

// Read the FileMapInfo information from the file.

bool FileMapInfo::init_from_file(int fd) {
  FileHeaderHelper file_helper(_full_path, _is_static);
  if (!file_helper.initialize(fd)) {
    fail_continue("Unable to read the file header.");
    return false;
  }
  GenericCDSFileMapHeader* gen_header = file_helper.get_generic_file_header();

  if (_is_static) {
    if (gen_header->_magic != CDS_ARCHIVE_MAGIC) {
      FileMapInfo::fail_continue("Not a base shared archive: %s", _full_path);
      return false;
    }
  } else {
    if (gen_header->_magic != CDS_DYNAMIC_ARCHIVE_MAGIC) {
      FileMapInfo::fail_continue("Not a top shared archive: %s", _full_path);
      return false;
    }
  }

  _header = (FileMapHeader*)os::malloc(gen_header->_header_size, mtInternal);
  os::lseek(fd, 0, SEEK_SET); // reset to begin of the archive
  size_t size = gen_header->_header_size;
  size_t n = ::read(fd, (void*)_header, (unsigned int)size);
  if (n != size) {
    fail_continue("Failed to read file header from the top archive file\n");
    return false;
  }

  if (header()->version() != CURRENT_CDS_ARCHIVE_VERSION) {
    log_info(cds)("_version expected: 0x%x", CURRENT_CDS_ARCHIVE_VERSION);
    log_info(cds)("           actual: 0x%x", header()->version());
    fail_continue("The shared archive file has the wrong version.");
    return false;
  }

  unsigned int base_offset = header()->base_archive_name_offset();
  unsigned int name_size = header()->base_archive_name_size();
  unsigned int header_size = header()->header_size();
  if (base_offset != 0 && name_size != 0) {
    if (header_size != base_offset + name_size) {
      log_info(cds)("_header_size: " UINT32_FORMAT, header_size);
      log_info(cds)("common_app_classpath_size: " UINT32_FORMAT, header()->common_app_classpath_prefix_size());
      log_info(cds)("base_archive_name_size: " UINT32_FORMAT, header()->base_archive_name_size());
      log_info(cds)("base_archive_name_offset: " UINT32_FORMAT, header()->base_archive_name_offset());
      FileMapInfo::fail_continue("The shared archive file has an incorrect header size.");
      return false;
    }
  }

  const char* actual_ident = header()->jvm_ident();

  if (actual_ident[JVM_IDENT_MAX-1] != 0) {
    FileMapInfo::fail_continue("JVM version identifier is corrupted.");
    return false;
  }

  char expected_ident[JVM_IDENT_MAX];
  get_header_version(expected_ident);
  if (strncmp(actual_ident, expected_ident, JVM_IDENT_MAX-1) != 0) {
    log_info(cds)("_jvm_ident expected: %s", expected_ident);
    log_info(cds)("             actual: %s", actual_ident);
    FileMapInfo::fail_continue("The shared archive file was created by a different"
                  " version or build of HotSpot");
    return false;
  }

  _file_offset = header()->header_size(); // accounts for the size of _base_archive_name

  size_t len = os::lseek(fd, 0, SEEK_END);

  for (int i = 0; i <= MetaspaceShared::last_valid_region; i++) {
    FileMapRegion* si = space_at(i);
    if (si->file_offset() > len || len - si->file_offset() < si->used()) {
      fail_continue("The shared archive file has been truncated.");
      return false;
    }
  }

  return true;
}

void FileMapInfo::seek_to_position(size_t pos) {
  if (os::lseek(_fd, (long)pos, SEEK_SET) < 0) {
    fail_stop("Unable to seek to position " SIZE_FORMAT, pos);
  }
}

// Read the FileMapInfo information from the file.
bool FileMapInfo::open_for_read() {
  if (_file_open) {
    return true;
  }
  log_info(cds)("trying to map %s", _full_path);
  int fd = os::open(_full_path, O_RDONLY | O_BINARY, 0);
  if (fd < 0) {
    if (errno == ENOENT) {
      fail_continue("Specified shared archive not found (%s)", _full_path);
    } else {
      fail_continue("Failed to open shared archive file (%s)",
                    os::strerror(errno));
    }
    return false;
  } else {
    log_info(cds)("Opened archive %s.", _full_path);
  }

  _fd = fd;
  _file_open = true;
  return true;
}

// Write the FileMapInfo information to the file.

void FileMapInfo::open_for_write() {
  LogMessage(cds) msg;
  if (msg.is_info()) {
    msg.info("Dumping shared data to file: ");
    msg.info("   %s", _full_path);
  }

#ifdef _WINDOWS  // On Windows, need WRITE permission to remove the file.
    chmod(_full_path, _S_IREAD | _S_IWRITE);
#endif

  // Use remove() to delete the existing file because, on Unix, this will
  // allow processes that have it open continued access to the file.
  remove(_full_path);
  int fd = os::open(_full_path, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0444);
  if (fd < 0) {
    fail_stop("Unable to create shared archive file %s: (%s).", _full_path,
              os::strerror(errno));
  }
  _fd = fd;
  _file_open = true;

  // Seek past the header. We will write the header after all regions are written
  // and their CRCs computed.
  size_t header_bytes = header()->header_size();

  header_bytes = align_up(header_bytes, MetaspaceShared::core_region_alignment());
  _file_offset = header_bytes;
  seek_to_position(_file_offset);
}

// Write the header to the file, seek to the next allocation boundary.

void FileMapInfo::write_header() {
  _file_offset = 0;
  seek_to_position(_file_offset);
  assert(is_file_position_aligned(), "must be");
  write_bytes(header(), header()->header_size());
}

size_t FileMapRegion::used_aligned() const {
  return align_up(used(), MetaspaceShared::core_region_alignment());
}

void FileMapRegion::init(int region_index, size_t mapping_offset, size_t size, bool read_only,
                         bool allow_exec, int crc) {
  _is_heap_region = HeapShared::is_heap_region(region_index);
  _is_bitmap_region = (region_index == MetaspaceShared::bm);
  _mapping_offset = mapping_offset;
  _used = size;
  _read_only = read_only;
  _allow_exec = allow_exec;
  _crc = crc;
  _mapped_from_file = false;
  _mapped_base = NULL;
}

void FileMapRegion::init_bitmaps(ArchiveHeapBitmapInfo oopmap, ArchiveHeapBitmapInfo ptrmap) {
  _oopmap_offset = oopmap._bm_region_offset;
  _oopmap_size_in_bits = oopmap._size_in_bits;

  _ptrmap_offset = ptrmap._bm_region_offset;
  _ptrmap_size_in_bits = ptrmap._size_in_bits;
}

BitMapView FileMapRegion::bitmap_view(bool is_oopmap) {
  char* bitmap_base = FileMapInfo::current_info()->map_bitmap_region();
  bitmap_base += is_oopmap ? _oopmap_offset : _ptrmap_offset;
  size_t size_in_bits = is_oopmap ? _oopmap_size_in_bits : _ptrmap_size_in_bits;
  return BitMapView((BitMap::bm_word_t*)(bitmap_base), size_in_bits);
}

BitMapView FileMapRegion::oopmap_view() {
  return bitmap_view(true);
}

BitMapView FileMapRegion::ptrmap_view() {
  assert(has_ptrmap(), "must be");
  return bitmap_view(false);
}

static const char* region_name(int region_index) {
  static const char* names[] = {
    "rw", "ro", "bm", "ca0", "ca1", "oa0", "oa1"
  };
  const int num_regions = sizeof(names)/sizeof(names[0]);
  assert(0 <= region_index && region_index < num_regions, "sanity");

  return names[region_index];
}

void FileMapRegion::print(outputStream* st, int region_index) {
  st->print_cr("============ region ============= %d \"%s\"", region_index, region_name(region_index));
  st->print_cr("- crc:                            0x%08x", _crc);
  st->print_cr("- read_only:                      %d", _read_only);
  st->print_cr("- allow_exec:                     %d", _allow_exec);
  st->print_cr("- is_heap_region:                 %d", _is_heap_region);
  st->print_cr("- is_bitmap_region:               %d", _is_bitmap_region);
  st->print_cr("- mapped_from_file:               %d", _mapped_from_file);
  st->print_cr("- file_offset:                    " SIZE_FORMAT_X, _file_offset);
  st->print_cr("- mapping_offset:                 " SIZE_FORMAT_X, _mapping_offset);
  st->print_cr("- used:                           " SIZE_FORMAT, _used);
  st->print_cr("- oopmap_offset:                  " SIZE_FORMAT_X, _oopmap_offset);
  st->print_cr("- oopmap_size_in_bits:            " SIZE_FORMAT, _oopmap_size_in_bits);
  st->print_cr("- mapped_base:                    " INTPTR_FORMAT, p2i(_mapped_base));
}

void FileMapInfo::write_region(int region, char* base, size_t size,
                               bool read_only, bool allow_exec) {
  Arguments::assert_is_dumping_archive();

  FileMapRegion* si = space_at(region);
  char* requested_base;
  size_t mapping_offset = 0;

  if (region == MetaspaceShared::bm) {
    requested_base = NULL; // always NULL for bm region
  } else if (size == 0) {
    // This is an unused region (e.g., a heap region when !INCLUDE_CDS_JAVA_HEAP)
    requested_base = NULL;
  } else if (HeapShared::is_heap_region(region)) {
    assert(!DynamicDumpSharedSpaces, "must be");
    requested_base = base;
    if (UseCompressedOops) {
      mapping_offset = (size_t)CompressedOops::encode_not_null(cast_to_oop(base));
    } else {
#if INCLUDE_G1GC
      mapping_offset = requested_base - (char*)G1CollectedHeap::heap()->reserved().start();
#endif
    }
    assert(mapping_offset == (size_t)(uint32_t)mapping_offset, "must be 32-bit only");
  } else {
    char* requested_SharedBaseAddress = (char*)MetaspaceShared::requested_base_address();
    requested_base = ArchiveBuilder::current()->to_requested(base);
    assert(requested_base >= requested_SharedBaseAddress, "must be");
    mapping_offset = requested_base - requested_SharedBaseAddress;
  }

  si->set_file_offset(_file_offset);
  int crc = ClassLoader::crc32(0, base, (jint)size);
  if (size > 0) {
    log_info(cds)("Shared file region (%-3s)  %d: " SIZE_FORMAT_W(8)
                   " bytes, addr " INTPTR_FORMAT " file offset 0x%08" PRIxPTR
                   " crc 0x%08x",
                   region_name(region), region, size, p2i(requested_base), _file_offset, crc);
  }

  si->init(region, mapping_offset, size, read_only, allow_exec, crc);

  if (base != NULL) {
    write_bytes_aligned(base, size);
  }
}

size_t FileMapInfo::set_bitmaps_offset(GrowableArray<ArchiveHeapBitmapInfo>* bitmaps, size_t curr_size) {
  for (int i = 0; i < bitmaps->length(); i++) {
    bitmaps->at(i)._bm_region_offset = curr_size;
    curr_size += bitmaps->at(i)._size_in_bytes;
  }
  return curr_size;
}

size_t FileMapInfo::write_bitmaps(GrowableArray<ArchiveHeapBitmapInfo>* bitmaps, size_t curr_offset, char* buffer) {
  for (int i = 0; i < bitmaps->length(); i++) {
    memcpy(buffer + curr_offset, bitmaps->at(i)._map, bitmaps->at(i)._size_in_bytes);
    curr_offset += bitmaps->at(i)._size_in_bytes;
  }
  return curr_offset;
}

char* FileMapInfo::write_bitmap_region(const CHeapBitMap* ptrmap,
                                       GrowableArray<ArchiveHeapBitmapInfo>* closed_bitmaps,
                                       GrowableArray<ArchiveHeapBitmapInfo>* open_bitmaps,
                                       size_t &size_in_bytes) {
  size_t size_in_bits = ptrmap->size();
  size_in_bytes = ptrmap->size_in_bytes();

  if (closed_bitmaps != NULL && open_bitmaps != NULL) {
    size_in_bytes = set_bitmaps_offset(closed_bitmaps, size_in_bytes);
    size_in_bytes = set_bitmaps_offset(open_bitmaps, size_in_bytes);
  }

  char* buffer = NEW_C_HEAP_ARRAY(char, size_in_bytes, mtClassShared);
  ptrmap->write_to((BitMap::bm_word_t*)buffer, ptrmap->size_in_bytes());
  header()->set_ptrmap_size_in_bits(size_in_bits);

  if (closed_bitmaps != NULL && open_bitmaps != NULL) {
    size_t curr_offset = write_bitmaps(closed_bitmaps, ptrmap->size_in_bytes(), buffer);
    write_bitmaps(open_bitmaps, curr_offset, buffer);
  }

  write_region(MetaspaceShared::bm, (char*)buffer, size_in_bytes, /*read_only=*/true, /*allow_exec=*/false);
  return buffer;
}

// Write out the given archive heap memory regions.  GC code combines multiple
// consecutive archive GC regions into one MemRegion whenever possible and
// produces the 'regions' array.
//
// If the archive heap memory size is smaller than a single dump time GC region
// size, there is only one MemRegion in the array.
//
// If the archive heap memory size is bigger than one dump time GC region size,
// the 'regions' array may contain more than one consolidated MemRegions. When
// the first/bottom archive GC region is a partial GC region (with the empty
// portion at the higher address within the region), one MemRegion is used for
// the bottom partial archive GC region. The rest of the consecutive archive
// GC regions are combined into another MemRegion.
//
// Here's the mapping from (archive heap GC regions) -> (GrowableArray<MemRegion> *regions).
//   + We have 1 or more archive heap regions: ah0, ah1, ah2 ..... ahn
//   + We have 1 or 2 consolidated heap memory regions: r0 and r1
//
// If there's a single archive GC region (ah0), then r0 == ah0, and r1 is empty.
// Otherwise:
//
// "X" represented space that's occupied by heap objects.
// "_" represented unused spaced in the heap region.
//
//
//    |ah0       | ah1 | ah2| ...... | ahn|
//    |XXXXXX|__ |XXXXX|XXXX|XXXXXXXX|XXXX|
//    |<-r0->|   |<- r1 ----------------->|
//            ^^^
//             |
//             +-- gap
size_t FileMapInfo::write_heap_regions(GrowableArray<MemRegion>* regions,
                                       GrowableArray<ArchiveHeapBitmapInfo>* bitmaps,
                                       int first_region_id, int max_num_regions) {
  assert(max_num_regions <= 2, "Only support maximum 2 memory regions");

  int arr_len = regions == NULL ? 0 : regions->length();
  if (arr_len > max_num_regions) {
    fail_stop("Unable to write archive heap memory regions: "
              "number of memory regions exceeds maximum due to fragmentation. "
              "Please increase java heap size "
              "(current MaxHeapSize is " SIZE_FORMAT ", InitialHeapSize is " SIZE_FORMAT ").",
              MaxHeapSize, InitialHeapSize);
  }

  size_t total_size = 0;
  for (int i = 0; i < max_num_regions; i++) {
    char* start = NULL;
    size_t size = 0;
    if (i < arr_len) {
      start = (char*)regions->at(i).start();
      size = regions->at(i).byte_size();
      total_size += size;
    }

    int region_idx = i + first_region_id;
    write_region(region_idx, start, size, false, false);
    if (size > 0) {
      int oopmap_idx = i * 2;
      int ptrmap_idx = i * 2 + 1;
      space_at(region_idx)->init_bitmaps(bitmaps->at(oopmap_idx),
                                         bitmaps->at(ptrmap_idx));
    }
  }
  return total_size;
}

// Dump bytes to file -- at the current file position.

void FileMapInfo::write_bytes(const void* buffer, size_t nbytes) {
  assert(_file_open, "must be");
  ssize_t n = os::write(_fd, buffer, (unsigned int)nbytes);
  if (n < 0 || (size_t)n != nbytes) {
    // If the shared archive is corrupted, close it and remove it.
    close();
    remove(_full_path);
    fail_stop("Unable to write to shared archive file.");
  }
  _file_offset += nbytes;
}

bool FileMapInfo::is_file_position_aligned() const {
  return _file_offset == align_up(_file_offset,
                                  MetaspaceShared::core_region_alignment());
}

// Align file position to an allocation unit boundary.

void FileMapInfo::align_file_position() {
  assert(_file_open, "must be");
  size_t new_file_offset = align_up(_file_offset,
                                    MetaspaceShared::core_region_alignment());
  if (new_file_offset != _file_offset) {
    _file_offset = new_file_offset;
    // Seek one byte back from the target and write a byte to insure
    // that the written file is the correct length.
    _file_offset -= 1;
    seek_to_position(_file_offset);
    char zero = 0;
    write_bytes(&zero, 1);
  }
}


// Dump bytes to file -- at the current file position.

void FileMapInfo::write_bytes_aligned(const void* buffer, size_t nbytes) {
  align_file_position();
  write_bytes(buffer, nbytes);
  align_file_position();
}

// Close the shared archive file.  This does NOT unmap mapped regions.

void FileMapInfo::close() {
  if (_file_open) {
    if (::close(_fd) < 0) {
      fail_stop("Unable to close the shared archive file.");
    }
    _file_open = false;
    _fd = -1;
  }
}


// JVM/TI RedefineClasses() support:
// Remap the shared readonly space to shared readwrite, private.
bool FileMapInfo::remap_shared_readonly_as_readwrite() {
  int idx = MetaspaceShared::ro;
  FileMapRegion* si = space_at(idx);
  if (!si->read_only()) {
    // the space is already readwrite so we are done
    return true;
  }
  size_t size = si->used_aligned();
  if (!open_for_read()) {
    return false;
  }
  char *addr = region_addr(idx);
  char *base = os::remap_memory(_fd, _full_path, si->file_offset(),
                                addr, size, false /* !read_only */,
                                si->allow_exec());
  close();
  // These have to be errors because the shared region is now unmapped.
  if (base == NULL) {
    log_error(cds)("Unable to remap shared readonly space (errno=%d).", errno);
    vm_exit(1);
  }
  if (base != addr) {
    log_error(cds)("Unable to remap shared readonly space (errno=%d).", errno);
    vm_exit(1);
  }
  si->set_read_only(false);
  return true;
}

// Memory map a region in the address space.
static const char* shared_region_name[] = { "ReadWrite", "ReadOnly", "Bitmap",
                                            "String1", "String2", "OpenArchive1", "OpenArchive2" };

MapArchiveResult FileMapInfo::map_regions(int regions[], int num_regions, char* mapped_base_address, ReservedSpace rs) {
  DEBUG_ONLY(FileMapRegion* last_region = NULL);
  intx addr_delta = mapped_base_address - header()->requested_base_address();

  // Make sure we don't attempt to use header()->mapped_base_address() unless
  // it's been successfully mapped.
  DEBUG_ONLY(header()->set_mapped_base_address((char*)(uintptr_t)0xdeadbeef);)

  for (int r = 0; r < num_regions; r++) {
    int idx = regions[r];
    MapArchiveResult result = map_region(idx, addr_delta, mapped_base_address, rs);
    if (result != MAP_ARCHIVE_SUCCESS) {
      return result;
    }
    FileMapRegion* si = space_at(idx);
    DEBUG_ONLY(if (last_region != NULL) {
        // Ensure that the OS won't be able to allocate new memory spaces between any mapped
        // regions, or else it would mess up the simple comparison in MetaspaceObj::is_shared().
        assert(si->mapped_base() == last_region->mapped_end(), "must have no gaps");
      }
      last_region = si;)
    log_info(cds)("Mapped %s region #%d at base " INTPTR_FORMAT " top " INTPTR_FORMAT " (%s)", is_static() ? "static " : "dynamic",
                  idx, p2i(si->mapped_base()), p2i(si->mapped_end()),
                  shared_region_name[idx]);

  }

  header()->set_mapped_base_address(header()->requested_base_address() + addr_delta);
  if (addr_delta != 0 && !relocate_pointers_in_core_regions(addr_delta)) {
    return MAP_ARCHIVE_OTHER_FAILURE;
  }

  return MAP_ARCHIVE_SUCCESS;
}

bool FileMapInfo::read_region(int i, char* base, size_t size, bool do_commit) {
  FileMapRegion* si = space_at(i);
  if (do_commit) {
    log_info(cds)("Commit %s region #%d at base " INTPTR_FORMAT " top " INTPTR_FORMAT " (%s)%s",
                  is_static() ? "static " : "dynamic", i, p2i(base), p2i(base + size),
                  shared_region_name[i], si->allow_exec() ? " exec" : "");
    if (!os::commit_memory(base, size, si->allow_exec())) {
      log_error(cds)("Failed to commit %s region #%d (%s)", is_static() ? "static " : "dynamic",
                     i, shared_region_name[i]);
      return false;
    }
  }
  if (os::lseek(_fd, (long)si->file_offset(), SEEK_SET) != (int)si->file_offset() ||
      read_bytes(base, size) != size) {
    return false;
  }

  if (VerifySharedSpaces && !region_crc_check(base, si->used(), si->crc())) {
    return false;
  }

  return true;
}

MapArchiveResult FileMapInfo::map_region(int i, intx addr_delta, char* mapped_base_address, ReservedSpace rs) {
  assert(!HeapShared::is_heap_region(i), "sanity");
  FileMapRegion* si = space_at(i);
  size_t size = si->used_aligned();
  char *requested_addr = mapped_base_address + si->mapping_offset();
  assert(si->mapped_base() == NULL, "must be not mapped yet");
  assert(requested_addr != NULL, "must be specified");

  si->set_mapped_from_file(false);

  if (MetaspaceShared::use_windows_memory_mapping()) {
    // Windows cannot remap read-only shared memory to read-write when required for
    // RedefineClasses, which is also used by JFR.  Always map windows regions as RW.
    si->set_read_only(false);
  } else if (JvmtiExport::can_modify_any_class() || JvmtiExport::can_walk_any_space() ||
             Arguments::has_jfr_option()) {
    // If a tool agent is in use (debugging enabled), or JFR, we must map the address space RW
    si->set_read_only(false);
  } else if (addr_delta != 0) {
    si->set_read_only(false); // Need to patch the pointers
  }

  if (MetaspaceShared::use_windows_memory_mapping() && rs.is_reserved()) {
    // This is the second time we try to map the archive(s). We have already created a ReservedSpace
    // that covers all the FileMapRegions to ensure all regions can be mapped. However, Windows
    // can't mmap into a ReservedSpace, so we just ::read() the data. We're going to patch all the
    // regions anyway, so there's no benefit for mmap anyway.
    if (!read_region(i, requested_addr, size, /* do_commit = */ true)) {
      log_info(cds)("Failed to read %s shared space into reserved space at " INTPTR_FORMAT,
                    shared_region_name[i], p2i(requested_addr));
      return MAP_ARCHIVE_OTHER_FAILURE; // oom or I/O error.
    }
  } else {
    // Note that this may either be a "fresh" mapping into unreserved address
    // space (Windows, first mapping attempt), or a mapping into pre-reserved
    // space (Posix). See also comment in MetaspaceShared::map_archives().
    char* base = os::map_memory(_fd, _full_path, si->file_offset(),
                                requested_addr, size, si->read_only(),
                                si->allow_exec(), mtClassShared);
    if (base != requested_addr) {
      log_info(cds)("Unable to map %s shared space at " INTPTR_FORMAT,
                    shared_region_name[i], p2i(requested_addr));
      _memory_mapping_failed = true;
      return MAP_ARCHIVE_MMAP_FAILURE;
    }
    si->set_mapped_from_file(true);
  }
  si->set_mapped_base(requested_addr);

  if (VerifySharedSpaces && !verify_region_checksum(i)) {
    return MAP_ARCHIVE_OTHER_FAILURE;
  }

  return MAP_ARCHIVE_SUCCESS;
}

// The return value is the location of the archive relocation bitmap.
char* FileMapInfo::map_bitmap_region() {
  FileMapRegion* si = space_at(MetaspaceShared::bm);
  if (si->mapped_base() != NULL) {
    return si->mapped_base();
  }
  bool read_only = true, allow_exec = false;
  char* requested_addr = NULL; // allow OS to pick any location
  char* bitmap_base = os::map_memory(_fd, _full_path, si->file_offset(),
                                     requested_addr, si->used_aligned(), read_only, allow_exec, mtClassShared);
  if (bitmap_base == NULL) {
    log_info(cds)("failed to map relocation bitmap");
    return NULL;
  }

  if (VerifySharedSpaces && !region_crc_check(bitmap_base, si->used(), si->crc())) {
    log_error(cds)("relocation bitmap CRC error");
    if (!os::unmap_memory(bitmap_base, si->used_aligned())) {
      fatal("os::unmap_memory of relocation bitmap failed");
    }
    return NULL;
  }

  si->set_mapped_base(bitmap_base);
  si->set_mapped_from_file(true);
  log_info(cds)("Mapped %s region #%d at base " INTPTR_FORMAT " top " INTPTR_FORMAT " (%s)",
                is_static() ? "static " : "dynamic",
                MetaspaceShared::bm, p2i(si->mapped_base()), p2i(si->mapped_end()),
                shared_region_name[MetaspaceShared::bm]);
  return bitmap_base;
}

// This is called when we cannot map the archive at the requested[ base address (usually 0x800000000).
// We relocate all pointers in the 2 core regions (ro, rw).
bool FileMapInfo::relocate_pointers_in_core_regions(intx addr_delta) {
  log_debug(cds, reloc)("runtime archive relocation start");
  char* bitmap_base = map_bitmap_region();

  if (bitmap_base == NULL) {
    return false; // OOM, or CRC check failure
  } else {
    size_t ptrmap_size_in_bits = header()->ptrmap_size_in_bits();
    log_debug(cds, reloc)("mapped relocation bitmap @ " INTPTR_FORMAT " (" SIZE_FORMAT " bits)",
                          p2i(bitmap_base), ptrmap_size_in_bits);

    BitMapView ptrmap((BitMap::bm_word_t*)bitmap_base, ptrmap_size_in_bits);

    // Patch all pointers in the mapped region that are marked by ptrmap.
    address patch_base = (address)mapped_base();
    address patch_end  = (address)mapped_end();

    // the current value of the pointers to be patched must be within this
    // range (i.e., must be between the requested base address and the address of the current archive).
    // Note: top archive may point to objects in the base archive, but not the other way around.
    address valid_old_base = (address)header()->requested_base_address();
    address valid_old_end  = valid_old_base + mapping_end_offset();

    // after patching, the pointers must point inside this range
    // (the requested location of the archive, as mapped at runtime).
    address valid_new_base = (address)header()->mapped_base_address();
    address valid_new_end  = (address)mapped_end();

    SharedDataRelocator patcher((address*)patch_base, (address*)patch_end, valid_old_base, valid_old_end,
                                valid_new_base, valid_new_end, addr_delta);
    ptrmap.iterate(&patcher);

    // The MetaspaceShared::bm region will be unmapped in MetaspaceShared::initialize_shared_spaces().

    log_debug(cds, reloc)("runtime archive relocation done");
    return true;
  }
}

size_t FileMapInfo::read_bytes(void* buffer, size_t count) {
  assert(_file_open, "Archive file is not open");
  size_t n = ::read(_fd, buffer, (unsigned int)count);
  if (n != count) {
    // Close the file if there's a problem reading it.
    close();
    return 0;
  }
  _file_offset += count;
  return count;
}

address FileMapInfo::decode_start_address(FileMapRegion* spc, bool with_current_oop_encoding_mode) {
  size_t offset = spc->mapping_offset();
  narrowOop n = CompressedOops::narrow_oop_cast(offset);
  if (with_current_oop_encoding_mode) {
    return cast_from_oop<address>(CompressedOops::decode_raw_not_null(n));
  } else {
    return cast_from_oop<address>(ArchiveHeapLoader::decode_from_archive(n));
  }
}

static MemRegion *closed_heap_regions = NULL;
static MemRegion *open_heap_regions = NULL;
static int num_closed_heap_regions = 0;
static int num_open_heap_regions = 0;

#if INCLUDE_CDS_JAVA_HEAP
bool FileMapInfo::has_heap_regions() {
  return (space_at(MetaspaceShared::first_closed_heap_region)->used() > 0);
}

// Returns the address range of the archived heap regions computed using the
// current oop encoding mode. This range may be different than the one seen at
// dump time due to encoding mode differences. The result is used in determining
// if/how these regions should be relocated at run time.
MemRegion FileMapInfo::get_heap_regions_range_with_current_oop_encoding_mode() {
  address start = (address) max_uintx;
  address end   = NULL;

  for (int i = MetaspaceShared::first_closed_heap_region;
           i <= MetaspaceShared::last_valid_region;
           i++) {
    FileMapRegion* si = space_at(i);
    size_t size = si->used();
    if (size > 0) {
      address s = start_address_as_decoded_with_current_oop_encoding_mode(si);
      address e = s + size;
      if (start > s) {
        start = s;
      }
      if (end < e) {
        end = e;
      }
    }
  }
  assert(end != NULL, "must have at least one used heap region");
  return MemRegion((HeapWord*)start, (HeapWord*)end);
}

void FileMapInfo::map_or_load_heap_regions() {
  bool success = false;

  if (can_use_heap_regions()) {
    if (ArchiveHeapLoader::can_map()) {
      success = map_heap_regions();
    } else if (ArchiveHeapLoader::can_load()) {
      success = ArchiveHeapLoader::load_heap_regions(this);
    } else {
      log_info(cds)("Cannot use CDS heap data. UseEpsilonGC, UseG1GC, UseSerialGC or UseParallelGC are required.");
    }
  }

  if (!success) {
    MetaspaceShared::disable_full_module_graph();
  }
}

bool FileMapInfo::can_use_heap_regions() {
  if (!has_heap_regions()) {
    return false;
  }
  if (JvmtiExport::should_post_class_file_load_hook() && JvmtiExport::has_early_class_hook_env()) {
    ShouldNotReachHere(); // CDS should have been disabled.
    // The archived objects are mapped at JVM start-up, but we don't know if
    // j.l.String or j.l.Class might be replaced by the ClassFileLoadHook,
    // which would make the archived String or mirror objects invalid. Let's be safe and not
    // use the archived objects. These 2 classes are loaded during the JVMTI "early" stage.
    //
    // If JvmtiExport::has_early_class_hook_env() is false, the classes of some objects
    // in the archived subgraphs may be replaced by the ClassFileLoadHook. But that's OK
    // because we won't install an archived object subgraph if the klass of any of the
    // referenced objects are replaced. See HeapShared::initialize_from_archived_subgraph().
  }

  log_info(cds)("CDS archive was created with max heap size = " SIZE_FORMAT "M, and the following configuration:",
                max_heap_size()/M);
  log_info(cds)("    narrow_klass_base = " PTR_FORMAT ", narrow_klass_shift = %d",
                p2i(narrow_klass_base()), narrow_klass_shift());
  log_info(cds)("    narrow_oop_mode = %d, narrow_oop_base = " PTR_FORMAT ", narrow_oop_shift = %d",
                narrow_oop_mode(), p2i(narrow_oop_base()), narrow_oop_shift());
  log_info(cds)("    heap range = [" PTR_FORMAT " - "  PTR_FORMAT "]",
                p2i(header()->heap_begin()), p2i(header()->heap_end()));

  log_info(cds)("The current max heap size = " SIZE_FORMAT "M, HeapRegion::GrainBytes = " SIZE_FORMAT,
                MaxHeapSize/M, HeapRegion::GrainBytes);
  log_info(cds)("    narrow_klass_base = " PTR_FORMAT ", narrow_klass_shift = %d",
                p2i(CompressedKlassPointers::base()), CompressedKlassPointers::shift());
  log_info(cds)("    narrow_oop_mode = %d, narrow_oop_base = " PTR_FORMAT ", narrow_oop_shift = %d",
                CompressedOops::mode(), p2i(CompressedOops::base()), CompressedOops::shift());
  log_info(cds)("    heap range = [" PTR_FORMAT " - "  PTR_FORMAT "]",
                UseCompressedOops ? p2i(CompressedOops::begin()) :
                                    UseG1GC ? p2i((address)G1CollectedHeap::heap()->reserved().start()) : 0L,
                UseCompressedOops ? p2i(CompressedOops::end()) :
                                    UseG1GC ? p2i((address)G1CollectedHeap::heap()->reserved().end()) : 0L);

  if (narrow_klass_base() != CompressedKlassPointers::base() ||
      narrow_klass_shift() != CompressedKlassPointers::shift()) {
    log_info(cds)("CDS heap data cannot be used because the archive was created with an incompatible narrow klass encoding mode.");
    return false;
  }
  return true;
}

// The address where the bottom of this shared heap region should be mapped
// at runtime
address FileMapInfo::heap_region_runtime_start_address(FileMapRegion* spc) {
  assert(UseSharedSpaces, "runtime only");
  spc->assert_is_heap_region();
  if (UseCompressedOops) {
    return start_address_as_decoded_from_archive(spc);
  } else {
    assert(is_aligned(spc->mapping_offset(), sizeof(HeapWord)), "must be");
    return header()->heap_begin() + spc->mapping_offset() + ArchiveHeapLoader::runtime_delta();
  }
}

void FileMapInfo::set_shared_heap_runtime_delta(ptrdiff_t delta) {
  if (UseCompressedOops) {
    ArchiveHeapLoader::init_narrow_oop_decoding(narrow_oop_base() + delta, narrow_oop_shift());
  } else {
    ArchiveHeapLoader::set_runtime_delta(delta);
  }
}

//
// Map the closed and open archive heap objects to the runtime java heap.
//
// The shared objects are mapped at (or close to ) the java heap top in
// closed archive regions. The mapped objects contain no out-going
// references to any other java heap regions. GC does not write into the
// mapped closed archive heap region.
//
// The open archive heap objects are mapped below the shared objects in
// the runtime java heap. The mapped open archive heap data only contains
// references to the shared objects and open archive objects initially.
// During runtime execution, out-going references to any other java heap
// regions may be added. GC may mark and update references in the mapped
// open archive objects.
void FileMapInfo::map_heap_regions_impl() {
  if (narrow_oop_mode() != CompressedOops::mode() ||
      narrow_oop_base() != CompressedOops::base() ||
      narrow_oop_shift() != CompressedOops::shift()) {
    log_info(cds)("CDS heap data needs to be relocated because the archive was created with an incompatible oop encoding mode.");
    _heap_pointers_need_patching = true;
  } else {
    if (UseCompressedOops) {
      MemRegion range = get_heap_regions_range_with_current_oop_encoding_mode();
      if (!CompressedOops::is_in(range)) {
        log_info(cds)("CDS heap data needs to be relocated because");
        log_info(cds)("the desired range " PTR_FORMAT " - "  PTR_FORMAT, p2i(range.start()), p2i(range.end()));
        log_info(cds)("is outside of the heap " PTR_FORMAT " - "  PTR_FORMAT, p2i(CompressedOops::begin()), p2i(CompressedOops::end()));
        _heap_pointers_need_patching = true;
      } else if (header()->heap_end() != CompressedOops::end()) {
        log_info(cds)("CDS heap data needs to be relocated to the end of the runtime heap to reduce fragmentation");
        _heap_pointers_need_patching = true;
      }
    } else {
      MemRegion range((HeapWord*)header()->heap_begin(), (HeapWord*)header()->heap_end());
      if (!G1CollectedHeap::heap()->reserved().contains(range)) {
        log_info(cds)("CDS heap data needs to be relocated because");
        log_info(cds)("the desired range " PTR_FORMAT " - "  PTR_FORMAT, p2i(range.start()), p2i(range.end()));
        log_info(cds)("is outside of the heap " PTR_FORMAT " - "  PTR_FORMAT,
            p2i((address)G1CollectedHeap::heap()->reserved().start()), p2i((address)G1CollectedHeap::heap()->reserved().end()));
        _heap_pointers_need_patching = true;
      } else if (header()->heap_end() != (address)G1CollectedHeap::heap()->reserved().end()) {
        log_info(cds)("CDS heap data needs to be relocated to the end of the runtime heap to reduce fragmentation");
        _heap_pointers_need_patching = true;
      }
    }
  }

  ptrdiff_t delta = 0;
  if (_heap_pointers_need_patching) {
    //   dumptime heap end  ------------v
    //   [      |archived heap regions| ]         run time heap end -----v
    //                                       [   |archived heap regions| ]
    //          ^
    //          D                                ^
    //                                           R
    //                                  |<-----delta-------------------->|
    //
    // At dump time, the archived heap regions were near the top of the heap.
    // At run time, if the heap ends at a different address, we need to
    // move them near to top of the run time heap. This can be done by
    // the simple math of adding the delta as shown above.
    //
    // Also: D = bottom of a heap region at dump time
    //       R = bottom of a heap region at run time
    //
    // FileMapRegion* spc = ...;
    //   address D = header()->heap_begin() + spc->mapping_offset();
    //   address R = D + delta;
    address dumptime_heap_end = header()->heap_end();
    address runtime_heap_end = UseCompressedOops ? CompressedOops::end() :
                                                   (address)G1CollectedHeap::heap()->reserved().end();
    delta = runtime_heap_end - dumptime_heap_end;
  }

  log_info(cds)("CDS heap data relocation delta = " INTX_FORMAT " bytes", delta);

  set_shared_heap_runtime_delta(delta);

  FileMapRegion* si = space_at(MetaspaceShared::first_closed_heap_region);
  address relocated_closed_heap_region_bottom = heap_region_runtime_start_address(si);

  if (!is_aligned(relocated_closed_heap_region_bottom, HeapRegion::GrainBytes)) {
    // Align the bottom of the closed archive heap regions at G1 region boundary.
    // This will avoid the situation where the highest open region and the lowest
    // closed region sharing the same G1 region. Otherwise we will fail to map the
    // open regions.
    size_t align = size_t(relocated_closed_heap_region_bottom) % HeapRegion::GrainBytes;
    delta -= align;
    log_info(cds)("CDS heap data needs to be relocated lower by a further " SIZE_FORMAT
                  " bytes to " INTX_FORMAT " to be aligned with HeapRegion::GrainBytes",
                  align, delta);
    set_shared_heap_runtime_delta(delta);
    relocated_closed_heap_region_bottom = heap_region_runtime_start_address(si);
    _heap_pointers_need_patching = true;
  }
  assert(is_aligned(relocated_closed_heap_region_bottom, HeapRegion::GrainBytes),
         "must be");

  if (_heap_pointers_need_patching) {
    char* bitmap_base = map_bitmap_region();
    if (bitmap_base == NULL) {
      log_info(cds)("CDS heap cannot be used because bitmap region cannot be mapped");
      _heap_pointers_need_patching = false;
      return;
    }
  }

  // Map the closed heap regions: GC does not write into these regions.
  if (map_heap_regions(MetaspaceShared::first_closed_heap_region,
                       MetaspaceShared::max_num_closed_heap_regions,
                       /*is_open_archive=*/ false,
                       &closed_heap_regions, &num_closed_heap_regions)) {
    ArchiveHeapLoader::set_closed_regions_mapped();

    // Now, map the open heap regions: GC can write into these regions.
    if (map_heap_regions(MetaspaceShared::first_open_heap_region,
                         MetaspaceShared::max_num_open_heap_regions,
                         /*is_open_archive=*/ true,
                         &open_heap_regions, &num_open_heap_regions)) {
      ArchiveHeapLoader::set_open_regions_mapped();
    }
  }
}

bool FileMapInfo::map_heap_regions() {
  map_heap_regions_impl();

  if (!ArchiveHeapLoader::closed_regions_mapped()) {
    assert(closed_heap_regions == NULL &&
           num_closed_heap_regions == 0, "sanity");
  }

  if (!ArchiveHeapLoader::open_regions_mapped()) {
    assert(open_heap_regions == NULL && num_open_heap_regions == 0, "sanity");
    return false;
  } else {
    return true;
  }
}

bool FileMapInfo::map_heap_regions(int first, int max,  bool is_open_archive,
                                   MemRegion** regions_ret, int* num_regions_ret) {
  MemRegion* regions = MemRegion::create_array(max, mtInternal);

  struct Cleanup {
    MemRegion* _regions;
    uint _length;
    bool _aborted;
    Cleanup(MemRegion* regions, uint length) : _regions(regions), _length(length), _aborted(true) { }
    ~Cleanup() { if (_aborted) { MemRegion::destroy_array(_regions, _length); } }
  } cleanup(regions, max);

  FileMapRegion* si;
  int num_regions = 0;

  for (int i = first;
           i < first + max; i++) {
    si = space_at(i);
    size_t size = si->used();
    if (size > 0) {
      HeapWord* start = (HeapWord*)heap_region_runtime_start_address(si);
      regions[num_regions] = MemRegion(start, size / HeapWordSize);
      num_regions ++;
      log_info(cds)("Trying to map heap data: region[%d] at " INTPTR_FORMAT ", size = " SIZE_FORMAT_W(8) " bytes",
                    i, p2i(start), size);
    }
  }

  if (num_regions == 0) {
    return false; // no archived java heap data
  }

  // Check that regions are within the java heap
  if (!G1CollectedHeap::heap()->check_archive_addresses(regions, num_regions)) {
    log_info(cds)("UseSharedSpaces: Unable to allocate region, range is not within java heap.");
    return false;
  }

  // allocate from java heap
  if (!G1CollectedHeap::heap()->alloc_archive_regions(
             regions, num_regions, is_open_archive)) {
    log_info(cds)("UseSharedSpaces: Unable to allocate region, java heap range is already in use.");
    return false;
  }

  // Map the archived heap data. No need to call MemTracker::record_virtual_memory_type()
  // for mapped regions as they are part of the reserved java heap, which is
  // already recorded.
  for (int i = 0; i < num_regions; i++) {
    si = space_at(first + i);
    char* addr = (char*)regions[i].start();
    char* base = os::map_memory(_fd, _full_path, si->file_offset(),
                                addr, regions[i].byte_size(), si->read_only(),
                                si->allow_exec());
    if (base == NULL || base != addr) {
      // dealloc the regions from java heap
      dealloc_heap_regions(regions, num_regions);
      log_info(cds)("UseSharedSpaces: Unable to map at required address in java heap. "
                    INTPTR_FORMAT ", size = " SIZE_FORMAT " bytes",
                    p2i(addr), regions[i].byte_size());
      return false;
    }

    if (VerifySharedSpaces && !region_crc_check(addr, regions[i].byte_size(), si->crc())) {
      // dealloc the regions from java heap
      dealloc_heap_regions(regions, num_regions);
      log_info(cds)("UseSharedSpaces: mapped heap regions are corrupt");
      return false;
    }

    si->set_mapped_base(base);
  }

  cleanup._aborted = false;
  // the shared heap data is mapped successfully
  *regions_ret = regions;
  *num_regions_ret = num_regions;
  return true;
}

void FileMapInfo::patch_heap_embedded_pointers() {
  if (!_heap_pointers_need_patching) {
    return;
  }

  log_info(cds)("patching heap embedded pointers");
  patch_heap_embedded_pointers(closed_heap_regions,
                               num_closed_heap_regions,
                               MetaspaceShared::first_closed_heap_region);

  patch_heap_embedded_pointers(open_heap_regions,
                               num_open_heap_regions,
                               MetaspaceShared::first_open_heap_region);
}

void FileMapInfo::patch_heap_embedded_pointers(MemRegion* regions, int num_regions,
                                               int first_region_idx) {
  char* bitmap_base = map_bitmap_region();
  assert(bitmap_base != NULL, "must have already been mapped");
  for (int i=0; i<num_regions; i++) {
    FileMapRegion* si = space_at(i + first_region_idx);
    ArchiveHeapLoader::patch_embedded_pointers(
      regions[i],
      (address)(space_at(MetaspaceShared::bm)->mapped_base()) + si->oopmap_offset(),
      si->oopmap_size_in_bits());
  }
}

// This internally allocates objects using vmClasses::Object_klass(), so it
// must be called after the Object_klass is loaded
void FileMapInfo::fixup_mapped_heap_regions() {
  assert(vmClasses::Object_klass_loaded(), "must be");
  // If any closed regions were found, call the fill routine to make them parseable.
  // Note that closed_heap_regions may be non-NULL even if no regions were found.
  if (num_closed_heap_regions != 0) {
    assert(closed_heap_regions != NULL,
           "Null closed_heap_regions array with non-zero count");
    G1CollectedHeap::heap()->fill_archive_regions(closed_heap_regions,
                                                  num_closed_heap_regions);
    // G1 marking uses the BOT for object chunking during marking in
    // G1CMObjArrayProcessor::process_slice(); for this reason we need to
    // initialize the BOT for closed archive regions too.
    G1CollectedHeap::heap()->populate_archive_regions_bot_part(closed_heap_regions,
                                                               num_closed_heap_regions);
  }

  // do the same for mapped open archive heap regions
  if (num_open_heap_regions != 0) {
    assert(open_heap_regions != NULL, "NULL open_heap_regions array with non-zero count");
    G1CollectedHeap::heap()->fill_archive_regions(open_heap_regions,
                                                  num_open_heap_regions);

    // Populate the open archive regions' G1BlockOffsetTableParts. That ensures
    // fast G1BlockOffsetTablePart::block_start operations for any given address
    // within the open archive regions when trying to find start of an object
    // (e.g. during card table scanning).
    G1CollectedHeap::heap()->populate_archive_regions_bot_part(open_heap_regions,
                                                               num_open_heap_regions);
  }
}

// dealloc the archive regions from java heap
void FileMapInfo::dealloc_heap_regions(MemRegion* regions, int num) {
  if (num > 0) {
    assert(regions != NULL, "Null archive regions array with non-zero count");
    G1CollectedHeap::heap()->dealloc_archive_regions(regions, num);
  }
}
#endif // INCLUDE_CDS_JAVA_HEAP

bool FileMapInfo::region_crc_check(char* buf, size_t size, int expected_crc) {
  int crc = ClassLoader::crc32(0, buf, (jint)size);
  if (crc != expected_crc) {
    fail_continue("Checksum verification failed.");
    return false;
  }
  return true;
}

bool FileMapInfo::verify_region_checksum(int i) {
  assert(VerifySharedSpaces, "sanity");
  size_t sz = space_at(i)->used();

  if (sz == 0) {
    return true; // no data
  } else {
    return region_crc_check(region_addr(i), sz, space_at(i)->crc());
  }
}

void FileMapInfo::unmap_regions(int regions[], int num_regions) {
  for (int r = 0; r < num_regions; r++) {
    int idx = regions[r];
    unmap_region(idx);
  }
}

// Unmap a memory region in the address space.

void FileMapInfo::unmap_region(int i) {
  assert(!HeapShared::is_heap_region(i), "sanity");
  FileMapRegion* si = space_at(i);
  char* mapped_base = si->mapped_base();
  size_t size = si->used_aligned();

  if (mapped_base != NULL) {
    if (size > 0 && si->mapped_from_file()) {
      log_info(cds)("Unmapping region #%d at base " INTPTR_FORMAT " (%s)", i, p2i(mapped_base),
                    shared_region_name[i]);
      if (!os::unmap_memory(mapped_base, size)) {
        fatal("os::unmap_memory failed");
      }
    }
    si->set_mapped_base(NULL);
  }
}

void FileMapInfo::assert_mark(bool check) {
  if (!check) {
    fail_stop("Mark mismatch while restoring from shared file.");
  }
}

void FileMapInfo::metaspace_pointers_do(MetaspaceClosure* it, bool use_copy) {
  if (use_copy) {
    _saved_shared_path_table.metaspace_pointers_do(it);
  } else {
    _shared_path_table.metaspace_pointers_do(it);
  }
}

FileMapInfo* FileMapInfo::_current_info = NULL;
FileMapInfo* FileMapInfo::_dynamic_archive_info = NULL;
bool FileMapInfo::_heap_pointers_need_patching = false;
SharedPathTable FileMapInfo::_shared_path_table;
SharedPathTable FileMapInfo::_saved_shared_path_table;
Array<u8>*      FileMapInfo::_saved_shared_path_table_array = NULL;
bool FileMapInfo::_validating_shared_path_table = false;
bool FileMapInfo::_memory_mapping_failed = false;
GrowableArray<const char*>* FileMapInfo::_non_existent_class_paths = NULL;

// Open the shared archive file, read and validate the header
// information (version, boot classpath, etc.).  If initialization
// fails, shared spaces are disabled and the file is closed. [See
// fail_continue.]
//
// Validation of the archive is done in two steps:
//
// [1] validate_header() - done here.
// [2] validate_shared_path_table - this is done later, because the table is in the RW
//     region of the archive, which is not mapped yet.
bool FileMapInfo::initialize() {
  assert(UseSharedSpaces, "UseSharedSpaces expected.");

  if (JvmtiExport::should_post_class_file_load_hook() && JvmtiExport::has_early_class_hook_env()) {
    // CDS assumes that no classes resolved in vmClasses::resolve_all()
    // are replaced at runtime by JVMTI ClassFileLoadHook. All of those classes are resolved
    // during the JVMTI "early" stage, so we can still use CDS if
    // JvmtiExport::has_early_class_hook_env() is false.
    FileMapInfo::fail_continue("CDS is disabled because early JVMTI ClassFileLoadHook is in use.");
    return false;
  }

  if (!Arguments::has_jimage()) {
    FileMapInfo::fail_continue("The shared archive file cannot be used with an exploded module build.");
    return false;
  }

  if (!open_for_read() || !init_from_file(_fd) || !validate_header()) {
    if (_is_static) {
      FileMapInfo::fail_continue("Initialize static archive failed.");
      return false;
    } else {
      FileMapInfo::fail_continue("Initialize dynamic archive failed.");
      if (AutoCreateSharedArchive) {
        DynamicDumpSharedSpaces = true;
        ArchiveClassesAtExit = Arguments::GetSharedDynamicArchivePath();
      }
      return false;
    }
  }

  return true;
}

char* FileMapInfo::region_addr(int idx) {
  FileMapRegion* si = space_at(idx);
  if (HeapShared::is_heap_region(idx)) {
    assert(DumpSharedSpaces, "The following doesn't work at runtime");
    return si->used() > 0 ?
          (char*)start_address_as_decoded_with_current_oop_encoding_mode(si) : NULL;
  } else {
    return si->mapped_base();
  }
}

// The 2 core spaces are RW->RO
FileMapRegion* FileMapInfo::first_core_space() const {
  return space_at(MetaspaceShared::rw);
}

FileMapRegion* FileMapInfo::last_core_space() const {
  return space_at(MetaspaceShared::ro);
}

void FileMapHeader::set_as_offset(char* p, size_t *offset) {
  *offset = ArchiveBuilder::current()->any_to_offset((address)p);
}

int FileMapHeader::compute_crc() {
  char* start = (char*)this;
  // start computing from the field after _header_size to end of base archive name.
  char* buf = (char*)&(_generic_header._header_size) + sizeof(_generic_header._header_size);
  size_t sz = header_size() - (buf - start);
  int crc = ClassLoader::crc32(0, buf, (jint)sz);
  return crc;
}

// This function should only be called during run time with UseSharedSpaces enabled.
bool FileMapHeader::validate() {
  if (_obj_alignment != ObjectAlignmentInBytes) {
    FileMapInfo::fail_continue("The shared archive file's ObjectAlignmentInBytes of %d"
                  " does not equal the current ObjectAlignmentInBytes of %d.",
                  _obj_alignment, ObjectAlignmentInBytes);
    return false;
  }
  if (_compact_strings != CompactStrings) {
    FileMapInfo::fail_continue("The shared archive file's CompactStrings setting (%s)"
                  " does not equal the current CompactStrings setting (%s).",
                  _compact_strings ? "enabled" : "disabled",
                  CompactStrings   ? "enabled" : "disabled");
    return false;
  }

  // This must be done after header validation because it might change the
  // header data
  const char* prop = Arguments::get_property("java.system.class.loader");
  if (prop != NULL) {
    warning("Archived non-system classes are disabled because the "
            "java.system.class.loader property is specified (value = \"%s\"). "
            "To use archived non-system classes, this property must not be set", prop);
    _has_platform_or_app_classes = false;
  }


  if (!_verify_local && BytecodeVerificationLocal) {
    //  we cannot load boot classes, so there's no point of using the CDS archive
    FileMapInfo::fail_continue("The shared archive file's BytecodeVerificationLocal setting (%s)"
                               " does not equal the current BytecodeVerificationLocal setting (%s).",
                               _verify_local ? "enabled" : "disabled",
                               BytecodeVerificationLocal ? "enabled" : "disabled");
    return false;
  }

  // For backwards compatibility, we don't check the BytecodeVerificationRemote setting
  // if the archive only contains system classes.
  if (_has_platform_or_app_classes
      && !_verify_remote // we didn't verify the archived platform/app classes
      && BytecodeVerificationRemote) { // but we want to verify all loaded platform/app classes
    FileMapInfo::fail_continue("The shared archive file was created with less restrictive "
                               "verification setting than the current setting.");
    // Pretend that we didn't have any archived platform/app classes, so they won't be loaded
    // by SystemDictionaryShared.
    _has_platform_or_app_classes = false;
  }

  // Java agents are allowed during run time. Therefore, the following condition is not
  // checked: (!_allow_archiving_with_java_agent && AllowArchivingWithJavaAgent)
  // Note: _allow_archiving_with_java_agent is set in the shared archive during dump time
  // while AllowArchivingWithJavaAgent is set during the current run.
  if (_allow_archiving_with_java_agent && !AllowArchivingWithJavaAgent) {
    FileMapInfo::fail_continue("The setting of the AllowArchivingWithJavaAgent is different "
                               "from the setting in the shared archive.");
    return false;
  }

  if (_allow_archiving_with_java_agent) {
    warning("This archive was created with AllowArchivingWithJavaAgent. It should be used "
            "for testing purposes only and should not be used in a production environment");
  }

  log_info(cds)("Archive was created with UseCompressedOops = %d, UseCompressedClassPointers = %d",
                          compressed_oops(), compressed_class_pointers());
  if (compressed_oops() != UseCompressedOops || compressed_class_pointers() != UseCompressedClassPointers) {
    FileMapInfo::fail_continue("Unable to use shared archive.\nThe saved state of UseCompressedOops and UseCompressedClassPointers is "
                               "different from runtime, CDS will be disabled.");
    return false;
  }

  if (!_use_optimized_module_handling) {
    MetaspaceShared::disable_optimized_module_handling();
    log_info(cds)("optimized module handling: disabled because archive was created without optimized module handling");
  }

  if (!_use_full_module_graph) {
    MetaspaceShared::disable_full_module_graph();
    log_info(cds)("full module graph: disabled because archive was created without full module graph");
  }

  return true;
}

bool FileMapInfo::validate_header() {
  if (!header()->validate()) {
    return false;
  }
  if (_is_static) {
    return true;
  } else {
    return DynamicArchive::validate(this);
  }
}

// Check if a given address is within one of the shared regions
bool FileMapInfo::is_in_shared_region(const void* p, int idx) {
  assert(idx == MetaspaceShared::ro ||
         idx == MetaspaceShared::rw, "invalid region index");
  char* base = region_addr(idx);
  if (p >= base && p < base + space_at(idx)->used()) {
    return true;
  }
  return false;
}

// Unmap mapped regions of shared space.
void FileMapInfo::stop_sharing_and_unmap(const char* msg) {
  MetaspaceShared::set_shared_metaspace_range(NULL, NULL, NULL);

  FileMapInfo *map_info = FileMapInfo::current_info();
  if (map_info) {
    map_info->fail_continue("%s", msg);
    for (int i = 0; i < MetaspaceShared::num_non_heap_regions; i++) {
      if (!HeapShared::is_heap_region(i)) {
        map_info->unmap_region(i);
      }
    }
    // Dealloc the archive heap regions only without unmapping. The regions are part
    // of the java heap. Unmapping of the heap regions are managed by GC.
    map_info->dealloc_heap_regions(open_heap_regions,
                                   num_open_heap_regions);
    map_info->dealloc_heap_regions(closed_heap_regions,
                                   num_closed_heap_regions);
  } else if (DumpSharedSpaces) {
    fail_stop("%s", msg);
  }
}

#if INCLUDE_JVMTI
ClassPathEntry** FileMapInfo::_classpath_entries_for_jvmti = NULL;

ClassPathEntry* FileMapInfo::get_classpath_entry_for_jvmti(int i, TRAPS) {
  if (i == 0) {
    // index 0 corresponds to the ClassPathImageEntry which is a globally shared object
    // and should never be deleted.
    return ClassLoader::get_jrt_entry();
  }
  ClassPathEntry* ent = _classpath_entries_for_jvmti[i];
  if (ent == NULL) {
    SharedClassPathEntry* scpe = shared_path(i);
    assert(scpe->is_jar(), "must be"); // other types of scpe will not produce archived classes

    const char* path = scpe->name();
    struct stat st;
    if (os::stat(path, &st) != 0) {
      char *msg = NEW_RESOURCE_ARRAY_IN_THREAD(THREAD, char, strlen(path) + 128);
      jio_snprintf(msg, strlen(path) + 127, "error in finding JAR file %s", path);
      THROW_MSG_(vmSymbols::java_io_IOException(), msg, NULL);
    } else {
      ent = ClassLoader::create_class_path_entry(THREAD, path, &st, false, false);
      if (ent == NULL) {
        char *msg = NEW_RESOURCE_ARRAY_IN_THREAD(THREAD, char, strlen(path) + 128);
        jio_snprintf(msg, strlen(path) + 127, "error in opening JAR file %s", path);
        THROW_MSG_(vmSymbols::java_io_IOException(), msg, NULL);
      }
    }

    MutexLocker mu(THREAD, CDSClassFileStream_lock);
    if (_classpath_entries_for_jvmti[i] == NULL) {
      _classpath_entries_for_jvmti[i] = ent;
    } else {
      // Another thread has beat me to creating this entry
      delete ent;
      ent = _classpath_entries_for_jvmti[i];
    }
  }

  return ent;
}

ClassFileStream* FileMapInfo::open_stream_for_jvmti(InstanceKlass* ik, Handle class_loader, TRAPS) {
  int path_index = ik->shared_classpath_index();
  assert(path_index >= 0, "should be called for shared built-in classes only");
  assert(path_index < (int)get_number_of_shared_paths(), "sanity");

  ClassPathEntry* cpe = get_classpath_entry_for_jvmti(path_index, CHECK_NULL);
  assert(cpe != NULL, "must be");

  Symbol* name = ik->name();
  const char* const class_name = name->as_C_string();
  const char* const file_name = ClassLoader::file_name_for_class_name(class_name,
                                                                      name->utf8_length());
  ClassLoaderData* loader_data = ClassLoaderData::class_loader_data(class_loader());
  ClassFileStream* cfs = cpe->open_stream_for_loader(THREAD, file_name, loader_data);
  assert(cfs != NULL, "must be able to read the classfile data of shared classes for built-in loaders.");
  log_debug(cds, jvmti)("classfile data for %s [%d: %s] = %d bytes", class_name, path_index,
                        cfs->source(), cfs->length());
  return cfs;
}

#endif
