/* mockturtle: C++ logic network library
 * Copyright (C) 2018-2020  EPFL
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*!
  \file aqfp_view.hpp
  \brief Constraints for AQFP technology

  \author Siang-Yun Lee
*/

#pragma once

#include <cstdint>
#include <stack>
#include <vector>
#include <cmath>

#include "../traits.hpp"
#include "../networks/detail/foreach.hpp"
#include "../utils/node_map.hpp"
#include "immutable_view.hpp"
#include "mockturtle/networks/mig.hpp"
#include "mockturtle/views/depth_view.hpp"

namespace mockturtle
{

struct aqfp_view_params
{
  bool update_on_add{true};
  bool update_on_modified{true};
  bool update_on_delete{true};

  uint32_t splitter_capacity{4u};
  uint32_t max_splitter_levels{2u};
};

/*! \brief Implements/Overwrites `foreach_fanout`, `depth`, `level`
 * `num_buffers`, `num_splitters`, `num_splitter_levels` methods for MIG network.
 *
 * This view computes the fanout of each node of the network.
 * It implements the network interface method `foreach_fanout`.  The
 * fanout are computed at construction and can be recomputed by
 * calling the `update_fanout` method.
 * 
 * The number of fanouts of each node is restricted to (`splitter_capacity`
 * to the power of `max_splitter_levels`).
 *
 * **Required network functions:**
 * - `foreach_node`
 * - `foreach_fanin`
 *
 */
template<typename Ntk, bool Check = false, bool has_fanout_interface = has_foreach_fanout_v<Ntk>>
class aqfp_view
{
};

template<typename Ntk, bool Check>
class aqfp_view<Ntk, Check, true> : public Ntk
{
public:
  aqfp_view( Ntk const& ntk, aqfp_view_params const& ps = {} ) : Ntk( ntk )
  {
    std::cerr << "[w] aqfp_view should not be built on top of fanout_view.\n";
    (void)ps;
  }
};

template<typename Ntk, bool Check>
class aqfp_view<Ntk, Check, false> : public Ntk
{
public:
  using storage = typename Ntk::storage;
  using node    = typename Ntk::node;
  using signal  = typename Ntk::signal;

  struct node_depth
  {
    node_depth( aqfp_view* p ): aqfp( p ) {}
    uint32_t operator()( depth_view<Ntk, node_depth> const& ntk, node const& n ) const
    {
      (void)ntk;
      return aqfp->num_splitter_levels( n ) + 1u;
    }
    aqfp_view* aqfp;
  };

  aqfp_view( Ntk const& ntk, aqfp_view_params const& ps = {} )
   : Ntk( ntk ), _fanout( ntk ), _ps( ps ), _max_fanout( std::pow( ps.splitter_capacity, ps.max_splitter_levels ) ), _node_depth( this ), _depth_view( ntk, _node_depth )
  {
    static_assert( !has_depth_v<Ntk> && !has_level_v<Ntk> && !has_update_levels_v<Ntk>, "Ntk already has depth interfaces" );
    static_assert( has_foreach_node_v<Ntk>, "Ntk does not implement the foreach_node method" );
    static_assert( has_foreach_fanin_v<Ntk>, "Ntk does not implement the foreach_fanin method" );

    if constexpr ( !std::is_same<Ntk, mig_network>::value )
    {
      std::cerr << "[w] Ntk is not mig_network type.\n";
    }

    update_fanout();

    if ( _ps.update_on_add )
    {
      Ntk::events().on_add.push_back( [this]( auto const& n ) {
        _fanout.resize();
        Ntk::foreach_fanin( n, [&, this]( auto const& f ) {
          _fanout[f].push_back( n );
        } );
      } );
    }

    if ( _ps.update_on_modified )
    {
      Ntk::events().on_modified.push_back( [this]( auto const& n, auto const& previous ) {
        (void)previous;
        for ( auto const& f : previous ) {
          _fanout[f].erase( std::remove( _fanout[f].begin(), _fanout[f].end(), n ), _fanout[f].end() );
        }
        Ntk::foreach_fanin( n, [&, this]( auto const& f ) {
          _fanout[f].push_back( n );
        } );
      } );
    }

    if ( _ps.update_on_delete )
    {
      Ntk::events().on_delete.push_back( [this]( auto const& n ) {
        _fanout[n].clear();
        Ntk::foreach_fanin( n, [&, this]( auto const& f ) {
          _fanout[f].erase( std::remove( _fanout[f].begin(), _fanout[f].end(), n ), _fanout[f].end() );
        } );
      } );
    }
  }

  template<typename Fn>
  void foreach_fanout( node const& n, Fn&& fn ) const
  {
    assert( n < this->size() );
    detail::foreach_element( _fanout[n].begin(), _fanout[n].end(), fn );
  }

  void update_fanout()
  {
    compute_fanout();
    _depth_view.update_levels();
  }

  /*! \brief Additional depth caused by the splitters of node `n` */
  uint32_t num_splitter_levels ( node const& n ) const
  {
    /* TODO: generalize for other `max_splitter_levels` values */
    return _fanout[n].size() > _ps.splitter_capacity ? 2u : (_fanout[n].size() > 1u ? 1u : 0u);
  }

  /* \brief Level of node `n` itself. Not the highest level of its splitters */
  uint32_t level ( node const& n ) const
  {
    return _depth_view.level( n ) - num_splitter_levels( n );
  }

  /* \brief Circuit depth */
  uint32_t depth() const
  {
    return _depth_view.depth();
  }

  /*! \brief Number of splitters at the fanout of node `n` */
  uint32_t num_splitters ( node const& n ) const
  {
    /* TODO: generalize for other `max_splitter_levels` values */
    if ( _fanout[n].size() <= 1u )
    {
      return 0u;
    }
    else if ( _fanout[n].size() <= _ps.splitter_capacity )
    {
      return 1u;
    }
    else
    {
      /* TODO: currently always fill up the second layer of splitters, but it's unnecessary */
      return _ps.splitter_capacity + 1u;
      //return std::floor( float( _fanout[n].size() ) / float( _ps.splitter_capacity ) ) + 1u;
    }
  }

  /*! \brief Get the number of buffers (including splitters) in the whole circuit */
  uint32_t num_buffers() const
  {
    uint32_t count = 0u;
    this->foreach_gate( [&]( auto const& n ){
      count += num_buffers( n );
    });
    return count;
  }

  /*! \brief Get the number of buffers (including splitters) between `n` and all its fanouts */
  uint32_t num_buffers( node const& n ) const
  {
    uint32_t count = 0u;
    auto const& fanouts = _fanout[n];
    uint32_t const nlevel = level( n ) + num_splitter_levels( n );
    for ( auto fo : fanouts )
    {
      assert( level( fo ) > nlevel );
      count += level( fo ) - nlevel - 1u;
    }
    return count + num_splitters( n );
  }

  void substitute_node( node const& old_node, signal const& new_signal )
  {
    std::cerr << "[e] aqfp_view has not been tested for network updating yet.\n";

    std::stack<std::pair<node, signal>> to_substitute;
    to_substitute.push( {old_node, new_signal} );

    while ( !to_substitute.empty() )
    {
      const auto [_old, _new] = to_substitute.top();
      to_substitute.pop();

      const auto parents = _fanout[_old];
      for ( auto n : parents )
      {
        if ( const auto repl = Ntk::replace_in_node( n, _old, _new ); repl )
        {
          to_substitute.push( *repl );
        }
      }

      /* check outputs */
      Ntk::replace_in_outputs( _old, _new );

      /* reset fan-in of old node */
      Ntk::take_out_node( _old );
    }
  }

private:
  void compute_fanout()
  {
    _fanout.reset();

    this->foreach_gate( [&]( auto const& n ){
        this->foreach_fanin( n, [&]( auto const& c ){
            auto& fanout = _fanout[c];
            if ( std::find( fanout.begin(), fanout.end(), n ) == fanout.end() )
            {
              fanout.push_back( n );
            }
          });
      });

    this->foreach_gate( [&]( auto const& n ){
        if constexpr ( Check )
        {
          if ( _fanout[n].size() > _max_fanout )
          {
            std::cerr << "[e] node " << n << " has too many (" << _fanout[n].size() << ") fanouts!\n";
          }
        }
        /* sort the fanouts by their level */
      });
  }

private:
  node_map<std::vector<node>, Ntk> _fanout;
  aqfp_view_params _ps;
  uint64_t _max_fanout;

  node_depth _node_depth;
  depth_view<Ntk, node_depth> _depth_view;
};

template<class T>
aqfp_view( T const&, aqfp_view_params const& ps = {} ) -> aqfp_view<T>;

} // namespace mockturtle
