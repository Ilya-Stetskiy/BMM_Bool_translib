#include "aig_to_anf.hpp"

#include <tracy/Tracy.hpp>

namespace bmm {

Result<Anf> aig_to_anf(const Aig& aig) {
    ZoneScoped;
    (void)aig;
    return fail<Anf>(ErrorCode::NotImplemented, "aig_to_anf: не реализовано");
}

}  // namespace bmm
