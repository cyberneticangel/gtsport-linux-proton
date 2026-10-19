#include <elfpatcher/general/SegmentFilter.hpp>
#include <elfpatcher/general/ElfConstants.hpp>

namespace Elfpatcher {

bool SegmentFilter::_isSceSpecificSegment(const std::uint32_t type) const {
    return type == PT_SCE_DYNLIBDATA
        || type == PT_OS_PROCPARAM
        || type == PT_OS_RELRO
        || (type >= PT_LOOS && type <= PT_HIOS);
}

bool SegmentFilter::ShouldSkip(const Domain::ProgramHeader& ph) const {
    if (ph.Type == PT_OS_PROCPARAM) return false;
    if (_isSceSpecificSegment(ph.Type)) return true;
    if (ph.Type == PT_DYNAMIC) return true;
    // PS4 executables name FreeBSD's loader (/libexec/ld-elf.so.1); the patcher writes the
    // host's own. The kernel uses the first PT_INTERP, so the original must go.
    if (ph.Type == PT_INTERP) return true;
    if (ph.Type == PT_NOTE && ph.MappedAddress == 0 && ph.FileSize > 0) return true;
    return false;
}

}
