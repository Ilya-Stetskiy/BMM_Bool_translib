#include "tt_to_anf.hpp"

#include <tracy/Tracy.hpp>

namespace bmm {

Result<Anf> tt_to_anf(const TruthTable& tt) {
    ZoneScoped;
    (void)tt;
    return fail<Anf>(ErrorCode::NotImplemented, "tt_to_anf: не реализовано");
}

}  // namespace bmm
