#include "anf_to_thr.hpp"

#include <tracy/Tracy.hpp>

namespace bmm {

Result<Thr> anf_to_thr(const Anf& anf) {
    ZoneScoped;
    (void)anf;
    return fail<Thr>(ErrorCode::NotImplemented, "anf_to_thr: не реализовано");
}

}  // namespace bmm
