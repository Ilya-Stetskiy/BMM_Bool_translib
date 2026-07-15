#include "anf_to_bdd.hpp"

#include <tracy/Tracy.hpp>

namespace bmm {

Result<Bdd> anf_to_bdd(const Anf& anf) {
    ZoneScoped;
    (void)anf;
    return fail<Bdd>(ErrorCode::NotImplemented, "anf_to_bdd: не реализовано");
}

}  // namespace bmm
