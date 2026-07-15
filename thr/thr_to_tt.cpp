#include "thr_to_tt.hpp"

#include <tracy/Tracy.hpp>

namespace bmm {

Result<TruthTable> thr_to_tt(const Thr& thr) {
    ZoneScoped;
    (void)thr;
    return fail<TruthTable>(ErrorCode::NotImplemented, "thr_to_tt: не реализовано");
}

}  // namespace bmm
