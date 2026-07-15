#include "thr_to_anf.hpp"

#include <tracy/Tracy.hpp>

namespace bmm {

Result<Anf> thr_to_anf(const Thr& thr) {
    ZoneScoped;
    (void)thr;
    return fail<Anf>(ErrorCode::NotImplemented, "thr_to_anf: не реализовано");
}

}  // namespace bmm
