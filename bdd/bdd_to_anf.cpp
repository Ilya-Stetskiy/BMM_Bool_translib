#include "bdd_to_anf.hpp"

#include <tracy/Tracy.hpp>

namespace bmm {

Result<Anf> bdd_to_anf(const Bdd& bdd) {
    ZoneScoped;
    (void)bdd;
    return fail<Anf>(ErrorCode::NotImplemented, "bdd_to_anf: не реализовано");
}

}  // namespace bmm
