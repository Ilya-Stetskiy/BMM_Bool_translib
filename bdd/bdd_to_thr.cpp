#include "bdd_to_thr.hpp"

#include <tracy/Tracy.hpp>

namespace bmm {

Result<Thr> bdd_to_thr(const Bdd& bdd) {
    ZoneScoped;
    (void)bdd;
    return fail<Thr>(ErrorCode::NotImplemented, "bdd_to_thr: не реализовано (см. TODO в bdd_to_thr.hpp)");
}

}  // namespace bmm
