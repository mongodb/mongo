// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"

#include <concepts>
#include <cstddef>
#include <iterator>
#include <type_traits>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Wraps a LiteParsedPipeline together with the BSON that the pipeline's stages were parsed from.
 * Use this whenever the backing BSON is a temporary that would otherwise die before the parsed
 * stages do — most commonly, subpipelines constructed inside a parent stage's lite parser.
 *
 * '_ownedStages' is declared before '_pipeline'. C++ initializes members in declaration order,
 * so the owned BSON copies exist before any BSONElement inside '_pipeline' references them.
 */
class OwnedLiteParsedPipeline {
public:
    OwnedLiteParsedPipeline(const NamespaceString& nss,
                            const std::vector<BSONObj>& pipelineStages,
                            const LiteParserOptions& options = {});

    // Moves already-parsed stages directly, without reparsing. Use this instead of the BSON-vector
    // constructor when the caller already holds StageSpecs (e.g. a desugarer re-assembling a
    // subpipeline) — reparsing loses information for stages whose original BSON is only a
    // placeholder (e.g. AST-only extension sub-stages), which can't round-trip through BSON.
    OwnedLiteParsedPipeline(NamespaceString nss, StageSpecs stages);

    OwnedLiteParsedPipeline(OwnedLiteParsedPipeline&&) noexcept = default;
    OwnedLiteParsedPipeline& operator=(OwnedLiteParsedPipeline&&) noexcept = default;

    OwnedLiteParsedPipeline(const OwnedLiteParsedPipeline& other);
    // Copy-assignment is not provided; construct a new OwnedLiteParsedPipeline explicitly.
    OwnedLiteParsedPipeline& operator=(const OwnedLiteParsedPipeline&) = delete;

    // Prefer operator-> or operator*. This is for when a caller has an OwnedLiteParsedPipeline
    // surrounded in a pointer, such that the caller doesn't need to write something like
    // (*_parsedPipeline).operator->() or &**_parsedPipeline.
    LiteParsedPipeline& pipeline() {
        return _pipeline;
    }

    // Prefer operator-> or operator*. This is for when a caller has an OwnedLiteParsedPipeline
    // surrounded in a pointer, such that the caller doesn't need to write something like
    // (*_parsedPipeline).operator->() or &**_parsedPipeline.
    const LiteParsedPipeline& pipeline() const {
        return _pipeline;
    }

    LiteParsedPipeline* operator->() {
        return &_pipeline;
    }

    const LiteParsedPipeline* operator->() const {
        return &_pipeline;
    }

    LiteParsedPipeline& operator*() {
        return _pipeline;
    }

    const LiteParsedPipeline& operator*() const {
        return _pipeline;
    }

    // The view NSS this pipeline was materialized from, if any. Set only for sub-pipelines created
    // by materializeViewSubpipeline() (e.g. the view pipeline materialized into a $graphLookup
    // stage), where the pipeline is parsed under the backing-collection NSS rather than the view's.
    // The current consumer is resolveInvolvedNamespacesImpl's cycle detection, which needs the view
    // NSS because the backing-collection NSS stored in _originalParseNss is not a view.
    const boost::optional<NamespaceString>& getViewNss() const {
        return _viewNss;
    }

    void setViewNss(NamespaceString nss) {
        _viewNss = std::move(nss);
    }

private:
    static std::vector<BSONObj> _makeStagesOwned(const std::vector<BSONObj>& pipelineStages);

    // Must be declared before '_pipeline': initializer-list order follows declaration order,
    // so the owned copies must exist before '_pipeline' parses them.
    std::vector<BSONObj> _ownedStages;
    LiteParsedPipeline _pipeline;
    boost::optional<NamespaceString> _viewNss;
};

/**
 * A view over a vector of LiteParsedPipelines that allows iterating and accessing and modifying
 * each pipeline in place, but never exposes the owning OwnedLiteParsedPipeline wrappers:
 * subpipelines cannot be reordered, swapped out, added, or removed through this view. This lets
 * stages that attach meaning to the pipeline order (e.g.
 * LiteParsedDocumentSourceOrderedNestedPipelines, which aligns pipeline names with subpipelines by
 * index) rely on that order being stable.
 *
 * 'Vector' is either std::vector<OwnedLiteParsedPipeline> or its const counterpart.
 */
template <typename Vector>
class OwnedLiteParsedPipelinesView {
    static_assert(
        std::is_same_v<std::remove_const_t<Vector>, std::vector<OwnedLiteParsedPipeline>>);

    static constexpr bool kIsConst = std::is_const_v<Vector>;

    using OwnedPipeline =
        std::conditional_t<kIsConst, const OwnedLiteParsedPipeline, OwnedLiteParsedPipeline>;
    using LitePipeline = std::conditional_t<kIsConst, const LiteParsedPipeline, LiteParsedPipeline>;

public:
    /**
     * A handle to a single subpipeline slot, returned by value from every element accessor. It
     * dereferences straight through to the LiteParsedPipeline (for in-place edits) but never
     * yields the owning OwnedLiteParsedPipeline wrapper, so the slot cannot be reassigned, moved
     * from, or swapped with another.
     */
    class reference {
    public:
        LitePipeline& operator*() const {
            return *_owned;
        }
        LitePipeline* operator->() const {
            return &*_owned;
        }

        // Wrapper-level accessors used by generic consumers (view cycle detection, desugaring).
        decltype(auto) pipeline() const {
            return _owned.pipeline();
        }
        const boost::optional<NamespaceString>& getViewNss() const {
            return _owned.getViewNss();
        }

    private:
        friend class OwnedLiteParsedPipelinesView;
        explicit reference(OwnedPipeline& owned) : _owned(owned) {}

        // Reference member: the handle is non-assignable, so std::swap, std::iter_swap and the
        // reordering algorithms fail to compile when applied to it.
        OwnedPipeline& _owned;
    };

    /**
     * Forward-only iterator yielding 'reference' handles by value. Deliberately not bidirectional:
     * std::reverse/std::sort reject it on iterator category alone, and the by-value proxy defeats
     * iter_swap-based reordering.
     */
    class iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = OwnedLiteParsedPipeline;
        using reference = OwnedLiteParsedPipelinesView::reference;
        using pointer = void;

        reference operator*() const {
            return reference{*_it};
        }
        iterator& operator++() {
            ++_it;
            return *this;
        }
        iterator operator++(int) {
            auto copy = *this;
            ++_it;
            return copy;
        }
        bool operator==(const iterator& other) const = default;

    private:
        friend class OwnedLiteParsedPipelinesView;
        using BaseIterator =
            std::conditional_t<kIsConst,
                               typename std::vector<OwnedLiteParsedPipeline>::const_iterator,
                               typename std::vector<OwnedLiteParsedPipeline>::iterator>;
        explicit iterator(BaseIterator it) : _it(it) {}
        BaseIterator _it;
    };

    explicit OwnedLiteParsedPipelinesView(Vector& pipelines) : _pipelines(pipelines) {}

    // In the const instantiation, constructs a const view from a mutable one (mirroring
    // T* -> const T*). In the mutable instantiation this is simply the copy constructor.
    OwnedLiteParsedPipelinesView(
        const OwnedLiteParsedPipelinesView<std::remove_const_t<Vector>>& other)
        : _pipelines(other._pipelines) {}

    iterator begin() const {
        return iterator{_pipelines.begin()};
    }
    iterator end() const {
        return iterator{_pipelines.end()};
    }
    reference operator[](size_t idx) const {
        return reference{_pipelines[idx]};
    }
    reference front() const {
        return reference{_pipelines.front()};
    }
    reference back() const {
        return reference{_pipelines.back()};
    }
    size_t size() const {
        return _pipelines.size();
    }
    bool empty() const {
        return _pipelines.empty();
    }

private:
    // Allows the const instantiation's converting constructor to read the mutable view's
    // underlying vector without exposing it publicly.
    template <typename>
    friend class OwnedLiteParsedPipelinesView;

    Vector& _pipelines;
};

// Compile-time regression checks for the stable-order contract:
// - callers can still edit each subpipeline in place through the handle:
static_assert(requires(MutableSubPipelinesView view) {
    { &*view.front() } -> std::same_as<LiteParsedPipeline*>;
    { &**view.begin() } -> std::same_as<LiteParsedPipeline*>;
    { view[0].pipeline() } -> std::same_as<LiteParsedPipeline&>;
});
static_assert(requires(ConstSubPipelinesView view) {
    { &*view.front() } -> std::same_as<const LiteParsedPipeline*>;
});
// - but the owning wrappers cannot be reassigned, swapped, or reordered through the view:
static_assert(!std::assignable_from<typename MutableSubPipelinesView::reference&,
                                    typename MutableSubPipelinesView::reference>);
static_assert(
    !std::assignable_from<typename MutableSubPipelinesView::reference&, OwnedLiteParsedPipeline&&>);
static_assert(!std::swappable_with<typename MutableSubPipelinesView::reference&,
                                   typename MutableSubPipelinesView::reference&>);
static_assert(!std::bidirectional_iterator<typename MutableSubPipelinesView::iterator>);
static_assert(!std::bidirectional_iterator<typename ConstSubPipelinesView::iterator>);

}  // namespace mongo
