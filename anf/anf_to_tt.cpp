#include "anf_to_tt.hpp"

#include <tracy/Tracy.hpp>

namespace bmm {

Result<TruthTable> anf_to_tt(const Anf& anf) {
    ZoneScoped;
    (void)anf;
    return fail<TruthTable>(ErrorCode::NotImplemented, "anf_to_tt: не реализовано");
}

}  // namespace bmm
