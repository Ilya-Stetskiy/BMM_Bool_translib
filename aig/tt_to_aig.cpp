#include "tt_to_aig.hpp"

#include <tracy/Tracy.hpp>

namespace bmm {

Result<Aig> tt_to_aig(const TruthTable& tt) {
    ZoneScoped;
    (void)tt;
    return fail<Aig>(ErrorCode::NotImplemented, "tt_to_aig: не реализовано");
}

}  // namespace bmm
