    #include "aig_to_anf.hpp"                                                                                                                 
                                                                                                                                              
    #include <tracy/Tracy.hpp>                                                                                                                
    #include <vector>                                                                                                                         
    #include <array>                                                                                                                          
    #include <mutex>                                                                                                                          
    #include <memory>                                                                                                                         
                                                                                                                                              
    namespace bmm {                                                                                                                           
                                                                                                                                              
    #if !BMM_HAVE_BRIAL                                                                                                                       
    Result<Anf> aig_to_anf(const Aig& /*aig*/) {                                                                                              
        return fail<Anf>(ErrorCode::Unsupported, "aig_to_anf: BRiAl не доступен в текущей сборке");                                           
    }                                                                                                                                         
    #else                                                                                                                                     
                                                                                                                                              
    namespace {                                                                                                                               
    BoolePolyRing& get_ring(uint32_t n) {                                                                                                     
        static std::unique_ptr<BoolePolyRing> active_ring;                                                                                    
        static uint32_t active_n = 0xffffffff;                                                                                                
        static std::mutex ring_mutex;                                                                                                         
        std::lock_guard<std::mutex> lock(ring_mutex);                                                                                         
        if (!active_ring || active_n != n) {                                                                                                  
            active_ring = std::make_unique<BoolePolyRing>(n == 0 ? 1 : n);                                                                    
            active_n = n;                                                                                                                     
        }                                                                                                                                     
        return *active_ring;                                                                                                                  
    }                                                                                                                                         
    } // namespace                                                                                                                            
                                                                                                                                              
    Result<Anf> aig_to_anf(const Aig& aig) {                                                                                                  
        ZoneScoped;                                                                                                                           
        const auto& net = aig.raw();                                                                                                          
        if (net.num_pos() != 1) {                                                                                                             
            return fail<Anf>(ErrorCode::Unsupported, "aig_to_anf: ожидается ровно один PO");                                                  
        }                                                                                                                                     
                                                                                                                                              
        const uint32_t n = aig.n_vars();                                                                                                      
        auto& ring = get_ring(n);                                                                                                             
                                                                                                                                              
        std::vector<BoolePolynomial> node_polys(net.size(), BoolePolynomial(ring));                                                           
                                                                                                                                              
        uint32_t const_node_idx = net.node_to_index(net.get_node(net.get_constant(false)));                                                   
        node_polys[const_node_idx] = BoolePolynomial(ring);                                                                                   
                                                                                                                                              
        uint32_t pi_counter = 0;                                                                                                              
        net.foreach_pi([&](auto node) {                                                                                                       
            uint32_t idx = net.node_to_index(node);                                                                                           
            node_polys[idx] = BoolePolynomial(ring.variable(pi_counter++));                                                                   
        });                                                                                                                                   
                                                                                                                                              
        const BoolePolynomial one_poly{BooleMonomial(ring)};                                                                                  
                                                                                                                                              
        net.foreach_gate([&](auto node) {                                                                                                     
            std::array<mockturtle::aig_network::signal, 2> fanins{};                                                                          
            uint32_t k = 0;                                                                                                                   
            net.foreach_fanin(node, [&](auto signal) { fanins[k++] = signal; });                                                              
                                                                                                                                              
            auto get_signal_poly = [&](mockturtle::aig_network::signal s) {                                                                   
                uint32_t child_idx = net.node_to_index(net.get_node(s));                                                                      
                const auto& p = node_polys[child_idx];                                                                                        
                if (net.is_complemented(s)) {                                                                                                 
                    return p + one_poly;                                                                                                      
                }                                                                                                                             
                return p;                                                                                                                     
            };                                                                                                                                
                                                                                                                                              
            auto p0 = get_signal_poly(fanins[0]);                                                                                             
            auto p1 = get_signal_poly(fanins[1]);                                                                                             
                                                                                                                                              
            if (p0.isZero() || p1.isZero()) {                                                                                                 
                node_polys[net.node_to_index(node)] = BoolePolynomial(ring);                                                                  
            } else if (p0.isOne()) {                                                                                                          
                node_polys[net.node_to_index(node)] = p1;                                                                                     
            } else if (p1.isOne()) {                                                                                                          
                node_polys[net.node_to_index(node)] = p0;                                                                                     
            } else if (p0 == p1) {                                                                                                            
                node_polys[net.node_to_index(node)] = p0;                                                                                     
            } else {                                                                                                                          
                node_polys[net.node_to_index(node)] = p0 * p1;                                                                                
            }
        });
  
        mockturtle::aig_network::signal po_sig;
        net.foreach_po([&](auto signal) { po_sig = signal; });
  
        uint32_t po_node_idx = net.node_to_index(net.get_node(po_sig));
        BoolePolynomial final_poly = node_polys[po_node_idx];
  
        if (net.is_complemented(po_sig)) {
            final_poly = final_poly + one_poly;
        }
  
        return ok<Anf>(Anf(std::move(final_poly), n));
    }
  
    #endif  // BMM_HAVE_BRIAL
  
    }  // namespace bmm