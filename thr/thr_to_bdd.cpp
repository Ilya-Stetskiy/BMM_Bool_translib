#include "thr_to_bdd.hpp"

#include <tracy/Tracy.hpp>

namespace bmm {

Result<Bdd> thr_to_bdd(const Thr& thr) {
    ZoneScoped;
    (void)thr;
    return fail<Bdd>(ErrorCode::NotImplemented, "thr_to_bdd: не реализовано");
}

}  // namespace bmm
