#include "bdd_to_tt.hpp"

#include <tracy/Tracy.hpp>

namespace bmm {

Result<TruthTable> bdd_to_tt(const Bdd& bdd) {
    ZoneScoped;
    (void)bdd;
    return fail<TruthTable>(ErrorCode::NotImplemented, "bdd_to_tt: не реализовано");
}

}  // namespace bmm
