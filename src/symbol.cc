// Copyright 2016 Uber Technologies, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./symbol.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <sstream>

#include "./posix.h"

namespace pyflame {
void ELF::Close() {
  if (addr_ != nullptr) {
    munmap(addr_, length_);
    addr_ = nullptr;
  }
}

// mmap the file
void ELF::Open(const std::string &target, Namespace *ns) {
  Close();
  int fd;
  if (ns != nullptr) {
    fd = ns->Open(target.c_str());
  } else {
    fd = open(target.c_str(), O_RDONLY);
  }
  if (fd == -1) {
    std::ostringstream ss;
    ss << "Failed to open ELF file " << target << ": " << strerror(errno);
    throw FatalException(ss.str());
  }
  length_ = lseek(fd, 0, SEEK_END);
  addr_ = mmap(nullptr, length_, PROT_READ, MAP_SHARED, fd, 0);
  pyflame::Close(fd);
  if (addr_ == MAP_FAILED) {
    std::ostringstream ss;
    ss << "Failed to mmap " << target << ": " << strerror(errno);
    throw FatalException(ss.str());
  }

  if (hdr()->e_ident[EI_MAG0] != ELFMAG0 ||
      hdr()->e_ident[EI_MAG1] != ELFMAG1 ||
      hdr()->e_ident[EI_MAG2] != ELFMAG2 ||
      hdr()->e_ident[EI_MAG3] != ELFMAG3) {
    std::ostringstream ss;
    ss << "File " << target << " does not have correct ELF magic header";
    throw FatalException(ss.str());
  }
  if (hdr()->e_ident[EI_CLASS] != ARCH_ELFCLASS) {
    throw FatalException("ELF class does not match host architecture");
  }
}

void ELF::Parse() {
  // skip the first section since it must be of type SHT_NULL
  for (uint16_t i = 1; i < hdr()->e_shnum; i++) {
    const shdr_t *s = shdr(i);
    switch (s->sh_type) {
      case SHT_STRTAB:
        if (strcmp(strtab(s->sh_name), ".dynstr") == 0) {
          dynstr_ = i;
        }
        break;
      case SHT_DYNSYM:
        dynsym_ = i;
        break;
      case SHT_DYNAMIC:
        dynamic_ = i;
        break;
    }
  }
  if (dynamic_ == -1) {
    throw FatalException("Failed to find section .dynamic");
  } else if (dynstr_ == -1) {
    throw FatalException("Failed to find section .dynstr");
  } else if (dynsym_ == -1) {
    throw FatalException("Failed to find section .dynsym");
  }
}

std::vector<std::string> ELF::NeededLibs() {
  // Get all of the strings
  std::vector<std::string> needed;
  const shdr_t *s = shdr(dynamic_);
  const shdr_t *d = shdr(dynstr_);
  for (uint16_t i = 0; i < s->sh_size / s->sh_entsize; i++) {
    const dyn_t *dyn =
        reinterpret_cast<const dyn_t *>(p() + s->sh_offset + i * s->sh_entsize);
    if (dyn->d_tag == DT_NEEDED) {
      needed.push_back(
          reinterpret_cast<const char *>(p() + d->sh_offset + dyn->d_un.d_val));
    }
  }
  return needed;
}

unsigned long ELF::GetThreadState(PyVersion *version) {
  bool have_version = false;
  unsigned long addr = 0;
  const shdr_t *s = shdr(dynsym_);
  const shdr_t *d = shdr(dynstr_);
  for (uint16_t i = 0; i < s->sh_size / s->sh_entsize; i++) {
    const sym_t *sym =
        reinterpret_cast<const sym_t *>(p() + s->sh_offset + i * s->sh_entsize);
    const char *name =
        reinterpret_cast<const char *>(p() + d->sh_offset + sym->st_name);
    if (!addr && strcmp(name, "_PyThreadState_Current") == 0) {
      addr = static_cast<unsigned long>(sym->st_value);
    } else if (!have_version) {
      if (strcmp(name, "PyString_Type") == 0) {
        // if we find PyString_Type, it's python 2
        have_version = true;
        *version = PyVersion::Py2;
      } else if (strcmp(name, "PyBytes_Type") == 0) {
        // if we find PyBytes_Type, it's python 3
        have_version = true;
        *version = PyVersion::Py3;
      }
    }
    if (have_version && addr) {
      break;
    }
  }
  return addr;
}
}  // namespace pyflame
