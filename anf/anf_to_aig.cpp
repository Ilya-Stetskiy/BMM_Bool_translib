#include "anf_to_aig.hpp"

#include <tracy/Tracy.hpp>

namespace bmm {

Result<Aig> anf_to_aig(const Anf& anf) {
    ZoneScoped;
    (void)anf;
    return fail<Aig>(ErrorCode::NotImplemented, "anf_to_aig: не реализовано");
}

}  // namespace bmm
