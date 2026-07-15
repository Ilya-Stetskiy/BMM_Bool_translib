#include "aig_to_thr.hpp"

#include <tracy/Tracy.hpp>

namespace bmm {

Result<Thr> aig_to_thr(const Aig& aig) {
    ZoneScoped;
    (void)aig;
    return fail<Thr>(ErrorCode::NotImplemented, "aig_to_thr: не реализовано");
}

}  // namespace bmm
