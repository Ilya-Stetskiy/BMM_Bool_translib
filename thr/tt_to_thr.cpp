#include "tt_to_thr.hpp"

#include <tracy/Tracy.hpp>

namespace bmm {

Result<Thr> tt_to_thr(const TruthTable& tt) {
    ZoneScoped;
    (void)tt;
    return fail<Thr>(ErrorCode::NotImplemented, "tt_to_thr: не реализовано");
}

}  // namespace bmm
