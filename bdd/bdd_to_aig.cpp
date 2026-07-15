#include "bdd_to_aig.hpp"

#include <tracy/Tracy.hpp>

namespace bmm {

Result<Aig> bdd_to_aig(const Bdd& bdd) {
    ZoneScoped;
    (void)bdd;
    return fail<Aig>(ErrorCode::NotImplemented, "bdd_to_aig: не реализовано");
}

}  // namespace bmm
