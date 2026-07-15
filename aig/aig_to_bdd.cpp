#include "aig_to_bdd.hpp"

#include <tracy/Tracy.hpp>

namespace bmm {

Result<Bdd> aig_to_bdd(const Aig& aig) {
    ZoneScoped;
    (void)aig;
    return fail<Bdd>(ErrorCode::NotImplemented, "aig_to_bdd: не реализовано");
}

}  // namespace bmm
