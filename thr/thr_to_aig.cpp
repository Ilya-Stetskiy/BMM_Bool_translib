#include "thr_to_aig.hpp"

#include <tracy/Tracy.hpp>

namespace bmm {

Result<Aig> thr_to_aig(const Thr& thr) {
    ZoneScoped;
    (void)thr;
    return fail<Aig>(ErrorCode::NotImplemented, "thr_to_aig: не реализовано");
}

}  // namespace bmm
