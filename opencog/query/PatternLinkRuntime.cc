/*
 * PatternLinkRuntime.cc
 *
 * Copyright (C) 2009, 2014, 2015 Linas Vepstas
 *
 * Author: Linas Vepstas <linasvepstas@gmail.com>  January 2009
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License v3 as
 * published by the Free Software Foundation and including the exceptions
 * at http://opencog.org/wiki/Licenses
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program; if not, write to:
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <opencog/util/Logger.h>

#include <opencog/atoms/pattern/BindLink.h>

#include <opencog/atomspace/AtomSpace.h>
#include <opencog/atoms/pattern/PatternUtils.h>

// #include "PatternMatchEngine.h"
#include <opencog/query/DefaultPatternMatchCB.h>

using namespace opencog;

#define DEBUG 1

/* ================================================================= */
/// A pass-through class, which wraps a regular callback, but captures
/// all of the different possible groundings that result.  This class is
/// used to piece together graphs out of multiple components.
class PMCGroundings : public PatternMatchCallback
{
	private:
		PatternMatchCallback& _cb;

	public:
		PMCGroundings(PatternMatchCallback& cb) : _cb(cb) {}

		// Pass all the calls straight through, except one.
		bool node_match(const Handle& node1, const Handle& node2) {
			return _cb.node_match(node1, node2);
		}
		bool variable_match(const Handle& node1, const Handle& node2) {
			return _cb.variable_match(node1, node2);
		}
		bool scope_match(const Handle& node1, const Handle& node2) {
			return _cb.scope_match(node1, node2);
		}
		bool link_match(const PatternTermPtr& link1, const Handle& link2) {
			return _cb.link_match(link1, link2);
		}
		bool post_link_match(const Handle& link1, const Handle& link2) {
			return _cb.post_link_match(link1, link2);
		}
		bool fuzzy_match(const Handle& h1, const Handle& h2) {
			return _cb.fuzzy_match(h1, h2);
		}
		bool evaluate_sentence(const Handle& link_h,
		                       const HandleMap &gnds)
		{
			return _cb.evaluate_sentence(link_h,gnds);
		}
		bool clause_match(const Handle& pattrn_link_h,
		                  const Handle& grnd_link_h,
		                  const HandleMap& term_gnds)
		{
			return _cb.clause_match(pattrn_link_h, grnd_link_h, term_gnds);
		}
		bool optional_clause_match(const Handle& pattrn,
		                           const Handle& grnd,
		                           const HandleMap& term_gnds)
		{
			return _cb.optional_clause_match(pattrn, grnd, term_gnds);
		}
		IncomingSet get_incoming_set(const Handle& h) {
			return _cb.get_incoming_set(h);
		}
		void push(void) { _cb.push(); }
		void pop(void) { _cb.pop(); }
		void set_pattern(const Variables& vars,
		                 const Pattern& pat)
		{
			_cb.set_pattern(vars, pat);
		}

		bool initiate_search(PatternMatchEngine* pme)
		{
			return _cb.initiate_search(pme);
		}

		bool search_finished(bool done)
		{
			return _cb.search_finished(done);
		}

		// This one we don't pass through. Instead, we collect the
		// groundings.
		bool grounding(const HandleMap &var_soln,
		               const HandleMap &term_soln)
		{
			_term_groundings.push_back(term_soln);
			_var_groundings.push_back(var_soln);
			return false;
		}

		HandleMapSeq _term_groundings;
		HandleMapSeq _var_groundings;
};

/**
 * Recursive evaluator/grounder/unifier of virtual link types.
 * The virtual links are in 'virtuals', a partial set of groundings
 * are in 'var_gnds' and 'term_gnds', and a collection of possible
 * groundings for disconnected graph components are in 'comp_var_gnds'
 * and 'comp_term_gnds'.
 *
 * Notes below explain the recursive step: how the various disconnected
 * components are brought together into a candidate grounding. That
 * candidate is then run through each of the virtual links.  If these
 * accept the grounding, then the callback is called to make the final
 * determination.
 *
 * The recursion step terminates when comp_var_gnds, comp_term_gnds
 * are empty, at which point the actual unification is done.
 *
 * Return false if no solution is found, true otherwise.
 */
static bool recursive_virtual(PatternMatchCallback& cb,
            const HandleSeq& virtuals,
            const HandleSeq& optionals,
            const HandleMap& var_gnds,
            const HandleMap& term_gnds,
            // copies, NOT references!
            HandleMapSeqSeq comp_var_gnds,
            HandleMapSeqSeq comp_term_gnds)
{
	// If we are done with the recursive step, then we have one of the
	// many combinatoric possibilities in the var_gnds and term_gnds
	// maps. Submit this grounding map to the virtual links, and see
	// what they've got to say about it.
	if (0 == comp_var_gnds.size())
	{
#ifdef DEBUG
		if (logger().is_fine_enabled())
		{
			logger().fine("Explore one possible combinatoric grounding "
			              "(var_gnds.size = %zu, term_gnds.size = %zu):",
			              var_gnds.size(), term_gnds.size());
			PatternMatchEngine::log_solution(var_gnds, term_gnds);
		}
#endif

		// Note, FYI, that if there are no virtual clauses at all,
		// then this loop falls straight-through, and the grounding
		// is reported as a match to the callback.  That is, the
		// virtuals only serve to reject possibilities.
		for (const Handle& virt : virtuals)
		{
			// At this time, we expect all virtual links to be in
			// one of two forms: either EvaluationLink's or
			// GreaterThanLink's. The EvaluationLinks should have
			// the structure
			//
			//   EvaluationLink
			//       GroundedPredicateNode "scm:blah"
			//       ListLink
			//           Arg1Atom
			//           Arg2Atom
			//
			// The GreaterThanLink's should have the "obvious" structure
			//
			//   GreaterThanLink
			//       Arg1Atom
			//       Arg2Atom
			//
			// In either case, one or more VariableNodes should appear
			// in the Arg atoms. So, we ground the args, and pass that
			// to the callback.

			bool match = cb.evaluate_sentence(virt, var_gnds);

			if (not match) return false;
		}

		Handle empty;
		for (const Handle& opt: optionals)
		{
			bool match = cb.optional_clause_match(opt, empty, var_gnds);
			if (not match) return false;
		}

		// Yay! We found one! We now have a fully and completely grounded
		// pattern! See what the callback thinks of it.
		return cb.grounding(var_gnds, term_gnds);
	}
#ifdef DEBUG
	LAZY_LOG_FINE << "Component recursion: num comp=" << comp_var_gnds.size();
#endif

	// Recurse over all components. If component k has N_k groundings,
	// and there are m components, then we have to explore all
	// N_0 * N_1 * N_2 * ... N_m possible combinations of groundings.
	// We do this recursively, by poping N_m off the back, and calling
	// ourselves.
	//
	// vg and vp will be the collection of all of the different possible
	// groundings for one of the components (well, its for component m,
	// in the above notation.) So the loop below tries every possibility.
	HandleMapSeq vg = comp_var_gnds.back();
	comp_var_gnds.pop_back();
	HandleMapSeq pg = comp_term_gnds.back();
	comp_term_gnds.pop_back();

	size_t ngnds = vg.size();
	for (size_t i=0; i<ngnds; i++)
	{
		// Given a set of groundings, tack on those for this component,
		// and recurse, with one less component. We need to make a copy,
		// of course.
		HandleMap rvg(var_gnds);
		HandleMap rpg(term_gnds);

		const HandleMap& cand_vg(vg[i]);
		const HandleMap& cand_pg(pg[i]);
		rvg.insert(cand_vg.begin(), cand_vg.end());
		rpg.insert(cand_pg.begin(), cand_pg.end());

		bool accept = recursive_virtual(cb, virtuals, optionals, rvg, rpg,
		                                comp_var_gnds, comp_term_gnds);

		// Halt recursion immediately if match is accepted.
		if (accept) return true;
	}
	return false;
}

/* ================================================================= */
/**
 * Ground (solve) a pattern; perform unification. That is, find one
 * or more groundings for the variables occuring in a collection of
 * clauses (a hypergraph). The hypergraph can be thought of as a
 * a 'predicate' which becomes 'true' when a grounding exists.
 *
 * The predicate is defined in terms of two hypergraphs: one is a
 * hypergraph defining a pattern to be grounded, and the other is a
 * list of bound variables in the first.
 *
 * The bound variables are, by convention, VariableNodes.  (The code in
 * the pattern match engine doesn't care whether the variable nodes are
 * actually of type VariableNode, and so can work with variables that
 * are any kind of node. However, the default callbacks do check for
 * this type. Thus, the restriction, by convention, that the variables
 * must be of type VariableNode.)  The list of bound variables is then
 * assumed to be listed using the ListLink type. So, for example:
 *
 *    ListLink
 *        VariableNode "variable 1"
 *        VariableNode "another variable"
 *
 * The pattern hypergraph is assumed to be a list of "clauses", where
 * each "clause" should be thought of as the tree defined by the outgoing
 * sets in it.  The below assumes that the list of clauses is specified
 * by means of an AndLink, so, for example:
 *
 *     AndLink
 *        SomeLink ....
 *        SomeOtherLink ...
 *
 * The clauses are assumed to be connected by variables, i.e. each
 * clause has a variable that also appears in some other clause.  Even
 * more strongly, it is assumed that there is just one connected
 * component; the code below throws an error if there is more than one
 * connected component.  The reason for this is to avoid unintended
 * combinatoric explosions: the grounding of any one (connected)
 * component is completely independent of the grounding of any other
 * component.  So, if there are two components, and one has N groundings
 * and the other has M groundings, then the two together trivially have
 * MxN groundings. Its worse if there are 4, 4... components. Rather
 * than stupidly reporting a result MxNx... times, we just throw an
 * error, and let the user decide what to do.
 *
 * The grounding proceeds by requiring each clause to match some part
 * of the atomspace (i.e. of the universe of hypergraphs stored in the
 * atomspace). When a solution is found, PatternMatchCallback::solution
 * method is called, and it is passed two maps: one mapping the bound
 * variables to their groundings, and the other mapping the pattern
 * clauses to their corresponding grounded clauses.
 *
 * Note: the pattern matcher itself doesn't use the atomspace, or care
 * if the groundings live in the atomspace; it can search anything.
 * However, the default callbacks do use the atomspace to find an
 * initial starting point for the search, and thus the search defacto
 * happens on the atomspace.  This restriction can be lifted by tweaking
 * the callback that initially launches the search.
 *
 * At this time, the list of clauses is understood to be a single
 * disjunct; that is, all of the clauses must be simultaneously
 * satisfied.  A future extension could allow the use of MatchOrLinks
 * to support multiple exclusive disjuncts. See the README for more info.
 */
bool PatternLink::satisfy(PatternMatchCallback& pmcb) const
{
	// If there is just one connected component, we don't have to
	// do anything special to find a grounding for it.  Proceed
	// in a direct fashion.
	if (_num_comps <= 1)
	{
		PatternMatchEngine pme(pmcb);

		debug_log();

		pme.set_pattern(_varlist, _pat);
		pmcb.set_pattern(_varlist, _pat);
		bool found = pmcb.initiate_search(&pme);

#ifdef DEBUG
		logger().fine("================= Done with Search =================");
#endif
		found = pmcb.search_finished(found);

		return found;
	}

	// If we are here, then we've got a knot in the center of it all.
	// Removing the virtual clauses from the hypergraph typically causes
	// the hypergraph to fall apart into multiple components, (i.e. none
	// are connected to one another). The virtual clauses tie all of
	// these back together into a single connected graph.
	//
	// There are several solution strategies possible at this point.
	// The one that we will pursue, for now, is to first ground all of
	// the distinct components individually, and then run each possible
	// grounding combination through the virtual link, for the final
	// accept/reject determination.

#ifdef DEBUG
	if (logger().is_fine_enabled())
	{
		logger().fine("VIRTUAL PATTERN: ====== "
		              "num comp=%zd num virts=%zd\n", _num_comps, _num_virts);
		logger().fine("Virtuals are:");
		size_t iii=0;
		for (const Handle& v : _virtual)
		{
			logger().fine("Virtual clause %zu of %zu:", iii, _num_virts);
			logger().fine(v->to_short_string());
			iii++;
		}
	}
#endif

	HandleMapSeqSeq comp_term_gnds;
	HandleMapSeqSeq comp_var_gnds;

	for (size_t i = 0; i < _num_comps; i++)
	{
#ifdef DEBUG
		LAZY_LOG_FINE << "BEGIN COMPONENT GROUNDING " << i+1
		              << " of " << _num_comps << ": ===========\n";
#endif

		PatternLinkPtr clp(PatternLinkCast(_component_patterns.at(i)));
		Pattern pat = clp->get_pattern();
		bool is_pure_optional = false;
		if (pat.mandatory.size() == 0 and pat.optionals.size() > 0)
			is_pure_optional = true;

		// Pass through the callbacks, collect up answers.
		PMCGroundings gcb(pmcb);
		clp->satisfy(gcb);

		// Special handling for disconnected pure optionals -- Returns false to
		// end the search if this disconnected pure optional is found
		if (is_pure_optional)
		{
			DefaultPatternMatchCB* dpmcb =
				dynamic_cast<DefaultPatternMatchCB*>(&pmcb);
			if (dpmcb->optionals_present()) return false;
		}
		else
		{
			// If there is no solution for one component, then no need
			// to try to solve the other components, their product
			// will have no solution.
			if (gcb._term_groundings.empty()) {
#ifdef DEBUG
				logger().fine("No solution for this component. "
				              "Abort search as no product solution may exist.");
#endif
				return false;
			}

			comp_var_gnds.push_back(gcb._var_groundings);
			comp_term_gnds.push_back(gcb._term_groundings);
		}
	}

	// And now, try grounding each of the virtual clauses.
#ifdef DEBUG
	LAZY_LOG_FINE << "BEGIN component recursion: ====================== "
	              << "num comp=" << comp_var_gnds.size()
	              << " num virts=" << _virtual.size();
#endif
	HandleMap empty_vg;
	HandleMap empty_pg;
	pmcb.set_pattern(_varlist, _pat);
	return recursive_virtual(pmcb, _virtual, _pat.optionals,
	                         empty_vg, empty_pg,
	                         comp_var_gnds, comp_term_gnds);
}

/* ===================== END OF FILE ===================== */
