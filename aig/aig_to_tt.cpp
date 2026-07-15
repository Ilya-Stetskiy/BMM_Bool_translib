#include "aig_to_tt.hpp"

#include <tracy/Tracy.hpp>

namespace bmm {

Result<TruthTable> aig_to_tt(const Aig& aig) {
    ZoneScoped;
    (void)aig;
    return fail<TruthTable>(ErrorCode::NotImplemented, "aig_to_tt: не реализовано");
}

}  // namespace bmm
