#include "tt_to_bdd.hpp"

#include <tracy/Tracy.hpp>

namespace bmm {

Result<Bdd> tt_to_bdd(const TruthTable& tt) {
    ZoneScoped;
    (void)tt;
    return fail<Bdd>(ErrorCode::NotImplemented, "tt_to_bdd: не реализовано");
}

}  // namespace bmm
