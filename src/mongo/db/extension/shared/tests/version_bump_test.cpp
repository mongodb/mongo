// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

// This unit test simulates version bumps to the Extensions API without modifying the frozen
// types/structs in public/api.h. Instead, it defines a small "future" slice of the API as it would
// look after two minor bumps and one major bump on top of the current 1.0:
//
//   minor 1: appends MongoExtensionWidget::do_work_v1_1 and introduces MongoExtensionSuperWidget.
//   minor 2: appends MongoExtensionWidget::do_extra_work_v1_2 and introduces
//            MongoExtensionWidgetRegistry.
//   major 2 (2.0): appends MongoExtensionWidget::get_registration_token_v2_0. register_widget
//            pulls the widget's token from it while registering, so the cross-type call is made
//            from inside extension code: the registry impl carries the negotiated version and gates
//            it. Because it is a cross-type contract between two pre-existing types, it lands on
//            the major boundary rather than another minor.
//
// Each simulated type gets a handle (a VTableAPI specialization mapped via the c_api_to_* traits),
// mirroring how real API handles are written. The tests exercise the CapabilityVersion machinery:
//
//   1. Version-conditional vtable constraints. assertVTableConstraints() only requires the vtable
//      slots that the negotiated capability version guarantees, so an extension compiled against an
//      older minor (whose vtable has the newer slots as nullptr) is accepted at old capabilities
//      and rejected at newer ones.
//   2. Per-type supported range. A type introduced at minor N cannot be wrapped by its handle when
//      a lower capability is negotiated (c_api_to_min_supported_capability / isSupported).
//   3. Runtime guards. Handles refuse to invoke vtable slots that their negotiated capability does
//      not cover.

#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/handle/handle.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mongo::extension {
namespace {

// ---------------------------------------------------------------------------
// Simulated minor version timeline.
// ---------------------------------------------------------------------------

// The highest capability version the simulated host supports: the "latest" of this simulated
// universe (major 2, minor 0).
constexpr CapabilityVersion kSimulatedLatestCapability(2, 0);

// Convenience capabilities used throughout the tests.
constexpr CapabilityVersion kCap10(1, 0);
constexpr CapabilityVersion kCap11(1, 1);
constexpr CapabilityVersion kCap12(1, 2);
constexpr CapabilityVersion kCap20(2, 0);
constexpr CapabilityVersion kCap21(2, 1);

}  // namespace

// The simulated C API structs are declared with C linkage, mirroring how the real API types in
// public/api.h are wrapped in an extern "C" block.
extern "C" {

// ---------------------------------------------------------------------------
// Simulated C API structs (what api.h would look like at minor 2).
// ---------------------------------------------------------------------------

/**
 * MongoExtensionWidget existed at minor 0. Its vtable gains three function pointers that a real
 * version bump would append at the end (backwards-compatible for ABI): two introduced by minor
 * bumps (do_work_v1_1, do_extra_work_v1_2) and one by the 2.0 major
 * (get_registration_token_v2_0). Extensions compiled against older versions leave the newer slots
 * unpopulated (nullptr).
 */
struct MongoExtensionWidget;
struct MongoExtensionWidgetVTable;
struct MongoExtensionWidgetRegistry;

typedef struct MongoExtensionWidgetVTable {
    void (*destroy)(MongoExtensionWidget* widget);
    MongoExtensionByteView (*get_name)(const MongoExtensionWidget* widget);
    MongoExtensionByteView (*bind)(const MongoExtensionWidget* widget);
    // Introduced in 1.1
    MongoExtensionByteView (*do_work_v1_1)(const MongoExtensionWidget* widget);
    // Introduced in 1.2
    void (*do_extra_work_v1_2)(MongoExtensionWidget* widget);
    // Introduced in 2.0. A read-only query the registry makes while registering the widget: only
    // the widget knows its own registration token.
    MongoExtensionByteView (*get_registration_token_v2_0)(const MongoExtensionWidget* widget);
} MongoExtensionWidgetVTable;

typedef struct MongoExtensionWidget {
    const struct MongoExtensionWidgetVTable* const vtable;
} MongoExtensionWidget;

/**
 * MongoExtensionSuperWidget did not exist until v1.1. A brand-new type is the other thing a
 * minor bump can introduce.
 */
struct MongoExtensionSuperWidget;
struct MongoExtensionSuperWidgetVTable;

typedef struct MongoExtensionSuperWidgetVTable {
    void (*destroy)(MongoExtensionSuperWidget* superWidget);
    int32_t (*get_num_cores)(const MongoExtensionSuperWidget* superWidget);
} MongoExtensionSuperWidgetVTable;

typedef struct MongoExtensionSuperWidget {
    const struct MongoExtensionSuperWidgetVTable* const vtable;
} MongoExtensionSuperWidget;

/**
 * MongoExtensionWidgetRegistry did not exist until 1.2.
 */
struct MongoExtensionWidgetRegistry;
struct MongoExtensionWidgetRegistryVTable;

typedef struct MongoExtensionWidgetRegistryVTable {
    void (*destroy)(MongoExtensionWidgetRegistry* registry);
    void (*register_widget)(MongoExtensionWidgetRegistry* registry, MongoExtensionWidget* widget);
    uint64_t (*num_registered)(const MongoExtensionWidgetRegistry* registry);
} MongoExtensionWidgetRegistryVTable;

typedef struct MongoExtensionWidgetRegistry {
    const struct MongoExtensionWidgetRegistryVTable* const vtable;
} MongoExtensionWidgetRegistry;

}  // extern "C"

namespace {

// ---------------------------------------------------------------------------
// Extension-side implementations, each compiled "against" a different minor.
// ---------------------------------------------------------------------------
template <typename T>
class WidgetVTableFuncs {
public:
    static void extDestroy(MongoExtensionWidget* widget) {
        delete static_cast<T*>(widget);
    }

    static MongoExtensionByteView extGetName(const MongoExtensionWidget* widget) {
        return stringViewAsByteView(static_cast<const T*>(widget)->getImpl().getName());
    }

    static MongoExtensionByteView extBind(const MongoExtensionWidget* widget) {
        return stringViewAsByteView(static_cast<const T*>(widget)->getImpl().bind());
    }

    static MongoExtensionByteView extDoWork(const MongoExtensionWidget* widget) {
        return stringViewAsByteView(static_cast<const T*>(widget)->getImpl().doWork_v1_1());
    }

    static void extDoExtraWork(MongoExtensionWidget* widget) {
        static_cast<T*>(widget)->getImpl().doExtraWork_v1_2();
    }

    static MongoExtensionByteView extGetRegistrationToken(const MongoExtensionWidget* widget) {
        return stringViewAsByteView(
            static_cast<const T*>(widget)->getImpl().getRegistrationToken());
    }
};
/**
 * Simulates an extension compiled against minor 0: it has no knowledge of any slot introduced after
 * minor 0, so do_work_v1_1 and do_extra_work_v1_2 are nullptr in its vtable.
 */
class WidgetLegacy {
public:
    std::string_view getName() const {
        return "widget";
    }

    std::string_view bind() const {
        return "bound";
    }
};


class WidgetImpl final : public MongoExtensionWidget {
public:
    static const MongoExtensionWidgetVTable VTABLE;

    WidgetImpl() : MongoExtensionWidget(&VTABLE) {}
    const WidgetLegacy& getImpl() const {
        return impl;
    }
    const WidgetLegacy& getImpl() {
        return impl;
    }

private:
    WidgetLegacy impl;
};

const MongoExtensionWidgetVTable WidgetImpl::VTABLE =
    MongoExtensionWidgetVTable{.destroy = &WidgetVTableFuncs<WidgetImpl>::extDestroy,
                               .get_name = &WidgetVTableFuncs<WidgetImpl>::extGetName,
                               .bind = &WidgetVTableFuncs<WidgetImpl>::extBind,
                               .do_work_v1_1 = nullptr,
                               .do_extra_work_v1_2 = nullptr};


class Widget_v1_1 : public WidgetLegacy {
public:
    std::string_view doWork_v1_1() const {
        return "doWork";
    }
};

/**
 * Simulates an extension compiled against minor 1: it knows about do_work_v1_1 but not
 * do_extra_work_v1_2.
 */
class WidgetImpl_v1_1 final : public MongoExtensionWidget {
public:
    static const MongoExtensionWidgetVTable VTABLE;

    WidgetImpl_v1_1() : MongoExtensionWidget(&VTABLE) {}

    const Widget_v1_1& getImpl() const {
        return impl;
    }
    const Widget_v1_1& getImpl() {
        return impl;
    }

private:
    Widget_v1_1 impl;
};

const MongoExtensionWidgetVTable WidgetImpl_v1_1::VTABLE =
    MongoExtensionWidgetVTable{.destroy = &WidgetVTableFuncs<WidgetImpl_v1_1>::extDestroy,
                               .get_name = &WidgetVTableFuncs<WidgetImpl_v1_1>::extGetName,
                               .bind = &WidgetVTableFuncs<WidgetImpl_v1_1>::extBind,
                               .do_work_v1_1 = &WidgetVTableFuncs<WidgetImpl_v1_1>::extDoWork,
                               .do_extra_work_v1_2 = nullptr};

class Widget_v1_2 : public Widget_v1_1 {
public:
    ~Widget_v1_2() {
        ++sDestroyCount;
    }
    void doExtraWork_v1_2() const {
        ++sExtraWorkCalls;
    }

    static int numExtraWorkCalls() {
        return sExtraWorkCalls;
    }

    static int numDestroyed() {
        return sDestroyCount;
    }

    static void resetCounters() {
        sDestroyCount = 0;
        sExtraWorkCalls = 0;
    }

private:
    static inline int sDestroyCount = 0;
    static inline int sExtraWorkCalls = 0;
};


/**
 * Simulates an extension compiled against minor 2: it implements every slot. Also counts its own
 * destruction so tests can verify owned-handle lifetime.
 */
class WidgetImpl_v1_2 final : public MongoExtensionWidget {
public:
    static const MongoExtensionWidgetVTable VTABLE;

    WidgetImpl_v1_2() : MongoExtensionWidget(&VTABLE) {}

    const Widget_v1_2& getImpl() const {
        return impl;
    }

    Widget_v1_2& getImpl() {
        return impl;
    }

    static int numExtraWorkCalls() {
        return Widget_v1_2::numExtraWorkCalls();
    }

    static int numDestroyed() {
        return Widget_v1_2::numDestroyed();
    }

    static void resetCounters() {
        Widget_v1_2::resetCounters();
    }

private:
    Widget_v1_2 impl;
};

const MongoExtensionWidgetVTable WidgetImpl_v1_2::VTABLE = MongoExtensionWidgetVTable{
    .destroy = &WidgetVTableFuncs<WidgetImpl_v1_2>::extDestroy,
    .get_name = &WidgetVTableFuncs<WidgetImpl_v1_2>::extGetName,
    .bind = &WidgetVTableFuncs<WidgetImpl_v1_2>::extBind,
    .do_work_v1_1 = &WidgetVTableFuncs<WidgetImpl_v1_2>::extDoWork,
    .do_extra_work_v1_2 = &WidgetVTableFuncs<WidgetImpl_v1_2>::extDoExtraWork};

class Widget_v2 : public Widget_v1_2 {
public:
    /**
     * Returns this widget's registration token. Only the widget itself knows this value; the
     * registry pulls it via get_registration_token_v2_0 while registering the widget.
     */
    std::string_view getRegistrationToken() const {
        return _registrationToken;
    }

private:
    // Per-instance: each widget carries a unique token, so the data the registry consumes differs
    // per widget.
    std::string _registrationToken = "widget_" + std::to_string(++sNextWidgetId);

    static inline int sNextWidgetId = 0;
};

/**
 * Simulates an extension compiled against 2.0: it implements every slot, including the
 * get_registration_token_v2_0 query the WidgetRegistry makes when registering the widget.
 */
class WidgetImpl_v2 final : public MongoExtensionWidget {
public:
    static const MongoExtensionWidgetVTable VTABLE;

    WidgetImpl_v2() : MongoExtensionWidget(&VTABLE) {}

    const Widget_v2& getImpl() const {
        return impl;
    }

    Widget_v2& getImpl() {
        return impl;
    }

private:
    Widget_v2 impl;
};

const MongoExtensionWidgetVTable WidgetImpl_v2::VTABLE = MongoExtensionWidgetVTable{
    .destroy = &WidgetVTableFuncs<WidgetImpl_v2>::extDestroy,
    .get_name = &WidgetVTableFuncs<WidgetImpl_v2>::extGetName,
    .bind = &WidgetVTableFuncs<WidgetImpl_v2>::extBind,
    .do_work_v1_1 = &WidgetVTableFuncs<WidgetImpl_v2>::extDoWork,
    .do_extra_work_v1_2 = &WidgetVTableFuncs<WidgetImpl_v2>::extDoExtraWork,
    .get_registration_token_v2_0 = &WidgetVTableFuncs<WidgetImpl_v2>::extGetRegistrationToken};

/**
 * Implements MongoExtensionSuperWidget (introduced at 1.1).
 */
class SuperWidgetImpl final : public MongoExtensionSuperWidget {
public:
    static const MongoExtensionSuperWidgetVTable VTABLE;

    SuperWidgetImpl() : MongoExtensionSuperWidget(&VTABLE) {}

    int32_t numCores() const {
        return 16;
    }

private:
    static void extDestroy(MongoExtensionSuperWidget* superWidget) {
        delete static_cast<SuperWidgetImpl*>(superWidget);
    }
    static int32_t extGetNumCores(const MongoExtensionSuperWidget* superWidget) {
        return static_cast<const SuperWidgetImpl*>(superWidget)->numCores();
    }
};

const MongoExtensionSuperWidgetVTable SuperWidgetImpl::VTABLE = MongoExtensionSuperWidgetVTable{
    .destroy = &SuperWidgetImpl::extDestroy, .get_num_cores = &SuperWidgetImpl::extGetNumCores};

}  // namespace

// The trait specializations and handles must be declared directly in mongo::extension: an explicit
// class template specialization cannot be nested inside an anonymous namespace.

// ---------------------------------------------------------------------------
// Host-side handles (VTableAPI specializations) for the simulated types.
// ---------------------------------------------------------------------------

/**
 * WidgetHandle wraps a MongoExtensionWidget. Widgets exist from minor 0 and remain valid through
 * the simulated latest minor. Whether a specific vtable slot is required depends on the negotiated
 * capability (see assertVTableConstraints), and the methods that reach into minor-introduced slots
 * refuse to run when the negotiated minor predates them.
 */
class WidgetAPI;
template <>
struct c_api_to_cpp_api<MongoExtensionWidget> {
    using CppApi_t = WidgetAPI;
};
template <>
struct c_api_to_min_supported_capability<MongoExtensionWidget>
    : std::integral_constant<uint32_t, static_cast<uint32_t>(CapabilityVersion(1, 0))> {};
template <>
struct c_api_to_max_supported_capability<MongoExtensionWidget>
    : std::integral_constant<uint32_t, static_cast<uint32_t>(kSimulatedLatestCapability)> {};

class WidgetAPI : public VTableAPI<MongoExtensionWidget> {
public:
    WidgetAPI(MongoExtensionWidget* widget, const CapabilityVersion& capability)
        : VTableAPI<MongoExtensionWidget>(widget, capability) {}

    std::string_view getName() const {
        return byteViewAsStringView(_vtable().get_name(get()));
    }

    std::string_view bind() const {
        return byteViewAsStringView(_vtable().bind(get()));
    }

    std::string_view doWork_v1_1() const {
        _assertFeatureSupported(kCap11);
        return byteViewAsStringView(_vtable().do_work_v1_1(get()));
    }

    void doExtraWork_v1_2() {
        _assertFeatureSupported(kCap12);
        _vtable().do_extra_work_v1_2(get());
    }

    // Pulls the registration token from the slot introduced in the 2.0 major. Invoked by the
    // registry while registering the widget. Only available when that major was negotiated.
    std::string_view getRegistrationToken() const {
        _assertFeatureSupported(kCap20);
        return byteViewAsStringView(_vtable().get_registration_token_v2_0(get()));
    }

    static void assertVTableConstraints(const VTable_t& vtable,
                                        const CapabilityVersion& capability) {
        tassert(ErrorCodes::InvalidExtensionVTable,
                "Widget 'get_name' is null",
                vtable.get_name != nullptr);
        tassert(
            ErrorCodes::InvalidExtensionVTable, "Widget 'bind' is null", vtable.bind != nullptr);
        if (capability >= kCap11) {
            tassert(ErrorCodes::InvalidExtensionVTable,
                    "Widget 'do_work_v1_1' is null",
                    vtable.do_work_v1_1 != nullptr);
        }
        if (capability >= kCap12) {
            tassert(ErrorCodes::InvalidExtensionVTable,
                    "Widget 'do_extra_work_v1_2' is null",
                    vtable.do_extra_work_v1_2 != nullptr);
        }
        if (capability >= kCap20) {
            tassert(ErrorCodes::InvalidExtensionVTable,
                    "Widget 'get_registration_token_v2_0' is null",
                    vtable.get_registration_token_v2_0 != nullptr);
        }
    }

private:
    void _assertFeatureSupported(CapabilityVersion capVersion) const {
        tassert(ErrorCodes::InvalidExtensionVTable,
                "Widget feature is not available at the negotiated API minor",
                _getCapabilityVersion() >= capVersion);
    }
};

using WidgetHandle = UnownedHandle<MongoExtensionWidget>;
using OwnedWidgetHandle = OwnedHandle<MongoExtensionWidget>;

/**
 * SuperWidgetHandle wraps a MongoExtensionSuperWidget, a type that did not exist before 1.1.
 * Its supported range begins at 1.1, so wrapping it at a negotiated 1.0 is rejected.
 */
class SuperWidgetAPI;
template <>
struct c_api_to_cpp_api<MongoExtensionSuperWidget> {
    using CppApi_t = SuperWidgetAPI;
};
template <>
struct c_api_to_min_supported_capability<MongoExtensionSuperWidget>
    : std::integral_constant<uint32_t, static_cast<uint32_t>(CapabilityVersion(1, 1))> {};
template <>
struct c_api_to_max_supported_capability<MongoExtensionSuperWidget>
    : std::integral_constant<uint32_t, static_cast<uint32_t>(kSimulatedLatestCapability)> {};

class SuperWidgetAPI : public VTableAPI<MongoExtensionSuperWidget> {
public:
    SuperWidgetAPI(MongoExtensionSuperWidget* superWidget, const CapabilityVersion& capability)
        : VTableAPI<MongoExtensionSuperWidget>(superWidget, capability) {}

    int32_t getNumCores() const {
        return _vtable().get_num_cores(get());
    }

    static void assertVTableConstraints(const VTable_t& vtable, const CapabilityVersion&) {
        tassert(ErrorCodes::InvalidExtensionVTable,
                "SuperWidget 'get_num_cores' is null",
                vtable.get_num_cores != nullptr);
    }
};

using SuperWidgetHandle = UnownedHandle<MongoExtensionSuperWidget>;

/**
 * WidgetRegistryHandle wraps a MongoExtensionWidgetRegistry, a type introduced in 1.2.
 */
class WidgetRegistryAPI;
template <>
struct c_api_to_cpp_api<MongoExtensionWidgetRegistry> {
    using CppApi_t = WidgetRegistryAPI;
};
template <>
struct c_api_to_min_supported_capability<MongoExtensionWidgetRegistry>
    : std::integral_constant<uint32_t, static_cast<uint32_t>(CapabilityVersion(1, 2))> {};
template <>
struct c_api_to_max_supported_capability<MongoExtensionWidgetRegistry>
    : std::integral_constant<uint32_t, static_cast<uint32_t>(kSimulatedLatestCapability)> {};

class WidgetRegistryAPI : public VTableAPI<MongoExtensionWidgetRegistry> {
public:
    WidgetRegistryAPI(MongoExtensionWidgetRegistry* registry, const CapabilityVersion& capability)
        : VTableAPI<MongoExtensionWidgetRegistry>(registry, capability) {}

    void registerWidget(MongoExtensionWidget* widget) {
        _vtable().register_widget(get(), widget);
    }

    uint64_t numRegistered() const {
        return _vtable().num_registered(get());
    }

    static void assertVTableConstraints(const VTable_t& vtable, const CapabilityVersion&) {
        tassert(ErrorCodes::InvalidExtensionVTable,
                "WidgetRegistry 'register_widget' is null",
                vtable.register_widget != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable,
                "WidgetRegistry 'num_registered' is null",
                vtable.num_registered != nullptr);
    }
};

using WidgetRegistryHandle = UnownedHandle<MongoExtensionWidgetRegistry>;

namespace {

/**
 * Implements MongoExtensionWidgetRegistry (introduced at 1.2). Carries the negotiated capability
 * version so its register_widget implementation can decide whether it may pull the widget's
 * registration token. Note, our tests track the capabilityVersion on handles, simulating the Host
 * side of the boundary. WidgetRegistryImpl could be assumed to be on the extension side of the
 * boundary. Both sides of the API boundary are responsible for tracking & propagating the
 * negotiated capabilityVersion independently once the extension is loaded.
 */
class WidgetRegistryImpl final : public MongoExtensionWidgetRegistry {
public:
    static const MongoExtensionWidgetRegistryVTable VTABLE;

    explicit WidgetRegistryImpl(const CapabilityVersion& capability)
        : MongoExtensionWidgetRegistry(&VTABLE), _registered(0), _capability(capability) {}

    void registerWidget(MongoExtensionWidget* widget) {
        ++_registered;
        // get_registration_token_v2_0 was introduced in 2.0: at a negotiated >= 2.0,
        // register_widget consumes the widget's own token as part of registering it;
        if (_capability >= kCap20) {
            UnownedHandle<MongoExtensionWidget> widgetHandle(widget, _capability);
            _tokens.push_back(std::string(widgetHandle->getRegistrationToken()));
        }
    }

    uint64_t numRegistered() const {
        return _registered;
    }

    const std::vector<std::string>& tokens() const {
        return _tokens;
    }

private:
    static void extDestroy(MongoExtensionWidgetRegistry* registry) {
        delete static_cast<WidgetRegistryImpl*>(registry);
    }
    static void extRegisterWidget(MongoExtensionWidgetRegistry* registry,
                                  MongoExtensionWidget* widget) {
        static_cast<WidgetRegistryImpl*>(registry)->registerWidget(widget);
    }
    static uint64_t extNumRegistered(const MongoExtensionWidgetRegistry* registry) {
        return static_cast<const WidgetRegistryImpl*>(registry)->numRegistered();
    }

    uint64_t _registered;
    const CapabilityVersion _capability;
    std::vector<std::string> _tokens;
};

const MongoExtensionWidgetRegistryVTable WidgetRegistryImpl::VTABLE =
    MongoExtensionWidgetRegistryVTable{.destroy = &WidgetRegistryImpl::extDestroy,
                                       .register_widget = &WidgetRegistryImpl::extRegisterWidget,
                                       .num_registered = &WidgetRegistryImpl::extNumRegistered};

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------

TEST(VersionBumpTest, CapabilityVersionBasics) {
    ASSERT_EQ(CapabilityVersion(1, 1).majorVersion(), 1u);
    ASSERT_EQ(CapabilityVersion(1, 1).minorVersion(), 1u);
    ASSERT_EQ(kCap12.majorVersion(), 1u);
    ASSERT_EQ(kCap12.minorVersion(), 2u);

    ASSERT_TRUE(kCap10 <= kCap11);
    ASSERT_TRUE(kCap11 >= kCap10);
    ASSERT_TRUE(kCap12 <= kCap12);
    ASSERT_TRUE(kSimulatedLatestCapability >= kCap11);

    // Minor version survives packing into the packed uint32 representation and back.
    const uint32_t packed = static_cast<uint32_t>(kCap11);
    const CapabilityVersion unpacked = CapabilityVersion::fromPackedValue(packed);
    ASSERT_EQ(unpacked.majorVersion(), 1u);
    ASSERT_EQ(unpacked.minorVersion(), 1u);
}

TEST(VersionBumpTest, WidgetV10WorksAtNegotiatedCapabilityV10) {
    WidgetImpl impl;
    // An extension compiled against minor 0 leaves the newer slots null. Construction succeeding at
    // negotiated (1,0) proves assertVTableConstraints does not require those slots.
    WidgetHandle widget(&impl, kCap10);
    widget->assertValid();
    ASSERT_EQ(widget->getName(), "widget");
    ASSERT_EQ(widget->bind(), "bound");
}

TEST(VersionBumpTest, WidgetV11WorksAtNegotiatedCapabilityV11) {
    WidgetImpl_v1_1 impl;
    WidgetHandle widget(&impl, kCap11);
    widget->assertValid();
    ASSERT_EQ(widget->getName(), "widget");
    ASSERT_EQ(widget->doWork_v1_1(), "doWork");
}

TEST(VersionBumpTest, WidgetV12WorksAtNegotiatedCapabilityV12) {
    WidgetImpl_v1_2::resetCounters();
    WidgetImpl_v1_2 impl;
    WidgetHandle widget(&impl, kCap12);
    widget->assertValid();
    ASSERT_EQ(widget->getName(), "widget");
    ASSERT_EQ(widget->doWork_v1_1(), "doWork");
    widget->doExtraWork_v1_2();
    ASSERT_EQ(WidgetImpl_v1_2::numExtraWorkCalls(), 1);
}

DEATH_TEST(VersionBumpTestDeathTest,
           WidgetV10AtCap11RejectedMissingDoWork,
           "'do_work_v1_1' is null") {
    WidgetImpl impl;
    // A minor 1 negotiation with an extension whose vtable lacks the minor-1 slot must fail at
    // handle construction: assertVTableConstraints is version-conditional on minor >= 1.
    WidgetHandle widget(&impl, kCap11);
    widget->assertValid();
}

DEATH_TEST(VersionBumpTestDeathTest,
           WidgetV11AtCap12RejectedMissingDoExtraWork,
           "'do_extra_work_v1_2' is null") {
    WidgetImpl_v1_1 impl;
    WidgetHandle widget(&impl, kCap12);
    widget->assertValid();
}

DEATH_TEST(VersionBumpTestDeathTest,
           WidgetV12DoWorkAtCap10Unavailable,
           "not available at the negotiated API minor") {
    WidgetImpl_v1_2 impl;
    WidgetHandle widget(&impl, kCap10);
    (void)widget->doWork_v1_1();
}

DEATH_TEST(VersionBumpTestDeathTest,
           WidgetV12DoExtraWorkAtCap11Unavailable,
           "not available at the negotiated API minor") {
    WidgetImpl_v1_2 impl;
    WidgetHandle widget(&impl, kCap11);
    widget->doExtraWork_v1_2();
}

DEATH_TEST(VersionBumpTestDeathTest,
           SuperWidgetAtCap10NotYetIntroduced,
           "is not supported by the requested API") {
    SuperWidgetImpl impl;
    SuperWidgetHandle superWidget(&impl, kCap10);
    superWidget->assertValid();
}

DEATH_TEST(VersionBumpTestDeathTest,
           WidgetRegistryAtCap11NotYetIntroduced,
           "is not supported by the requested API") {
    WidgetRegistryImpl impl(kCap12);
    WidgetRegistryHandle registry(&impl, kCap11);
    registry->assertValid();
}

TEST(VersionBumpTest, SuperWidgetWorksOnceIntroduced) {
    SuperWidgetImpl impl;
    SuperWidgetHandle superWidget(&impl, kCap11);
    superWidget->assertValid();
    ASSERT_EQ(superWidget->getNumCores(), 16);

    SuperWidgetHandle superWidgetLater(&impl, kCap12);
    superWidgetLater->assertValid();
    ASSERT_EQ(superWidgetLater->getNumCores(), 16);
}

TEST(VersionBumpTest, WidgetRegistryWorksOnceIntroduced) {
    WidgetRegistryImpl registry(kCap12);
    WidgetRegistryHandle registryHandle(&registry, kCap12);
    registryHandle->assertValid();
    ASSERT_EQ(registryHandle->numRegistered(), 0u);

    WidgetImpl_v1_2 widget;
    registryHandle->registerWidget(&widget);
    registryHandle->registerWidget(&widget);
    ASSERT_EQ(registryHandle->numRegistered(), 2u);
}

TEST(VersionBumpTest, SupportedCapabilityRanges) {
    // Widgets exist across the entire simulated range [1.0, 2.0].
    ASSERT_TRUE(WidgetAPI::isSupported(kCap10));
    ASSERT_TRUE(WidgetAPI::isSupported(kCap11));
    ASSERT_TRUE(WidgetAPI::isSupported(kCap12));
    ASSERT_TRUE(WidgetAPI::isSupported(kCap20));
    // But they cannot be wrapped at a capability the handle does not support.
    ASSERT_FALSE(WidgetAPI::isSupported(kCap21));

    // SuperWidgets range over [1.1, 2.0].
    ASSERT_FALSE(SuperWidgetAPI::isSupported(kCap10));
    ASSERT_TRUE(SuperWidgetAPI::isSupported(kCap11));
    ASSERT_TRUE(SuperWidgetAPI::isSupported(kCap12));
    ASSERT_TRUE(SuperWidgetAPI::isSupported(kCap20));
    ASSERT_FALSE(SuperWidgetAPI::isSupported(kCap21));

    // The WidgetRegistry spans [1.2, 2.0]: introduced in 1.2, still valid at the latest major.
    ASSERT_FALSE(WidgetRegistryAPI::isSupported(kCap10));
    ASSERT_FALSE(WidgetRegistryAPI::isSupported(kCap11));
    ASSERT_TRUE(WidgetRegistryAPI::isSupported(kCap12));
    ASSERT_TRUE(WidgetRegistryAPI::isSupported(kCap20));
    ASSERT_FALSE(WidgetRegistryAPI::isSupported(kCap21));
}

TEST(VersionBumpTest, WidgetV20GetRegistrationTokenViaHandle) {
    WidgetImpl_v2 widget;
    WidgetHandle handle(&widget, kCap20);
    handle->assertValid();
    ASSERT_EQ(handle->getRegistrationToken(), widget.getImpl().getRegistrationToken());
}

TEST(VersionBumpTest, RegistryStoresWidgetTokenAtCap20) {
    WidgetRegistryImpl registry(kCap20);
    WidgetRegistryHandle registryHandle(&registry, kCap20);
    WidgetImpl_v2 widgetA;
    WidgetImpl_v2 widgetB;
    registryHandle->registerWidget(&widgetA);
    registryHandle->registerWidget(&widgetA);
    registryHandle->registerWidget(&widgetB);
    ASSERT_EQ(registryHandle->numRegistered(), 3u);
    // register_widget pulled each widget's own token while registering it.
    ASSERT_EQ(registry.tokens().size(), 3u);
    ASSERT_EQ(registry.tokens()[0], std::string(widgetA.getImpl().getRegistrationToken()));
    ASSERT_EQ(registry.tokens()[1], std::string(widgetA.getImpl().getRegistrationToken()));
    ASSERT_EQ(registry.tokens()[2], std::string(widgetB.getImpl().getRegistrationToken()));
    // The registry consumed widget-specific data: distinct widgets yield distinct tokens.
    ASSERT_NE(registry.tokens()[0], registry.tokens()[2]);
}

TEST(VersionBumpTest, RegistryStoresNoTokenAtCap12) {
    // get_registration_token_v2_0 was introduced in 2.0, so a 1.2 session registers the widget
    // without pulling its token -- the same binary provides the slot, but the negotiated version
    // forbids reaching into it from register_widget.
    WidgetRegistryImpl registry(kCap12);
    WidgetRegistryHandle registryHandle(&registry, kCap12);
    WidgetImpl_v2 widget;
    registryHandle->registerWidget(&widget);
    ASSERT_EQ(registryHandle->numRegistered(), 1u);
    ASSERT_TRUE(registry.tokens().empty());
}

DEATH_TEST(VersionBumpTestDeathTest,
           WidgetV12GetRegistrationTokenAtCap20MissingSlot,
           "'get_registration_token_v2_0' is null") {
    // A widget compiled before 2.0 leaves get_registration_token_v2_0 null. Registering it at a
    // negotiated 2.0 is rejected inside register_widget when it wraps the widget to pull the token:
    // the version-conditional vtable constraints fire.
    WidgetRegistryImpl registry(kCap20);
    WidgetRegistryHandle registryHandle(&registry, kCap20);
    WidgetImpl_v1_2 widget;
    registryHandle->registerWidget(&widget);
}

TEST(VersionBumpTest, OwnedWidgetHandleDestroysAtCapability) {
    WidgetImpl_v1_2::resetCounters();
    {
        OwnedWidgetHandle widget(new WidgetImpl_v1_2(), kCap12);
        ASSERT_EQ(WidgetImpl_v1_2::numDestroyed(), 0);
        (void)widget->doWork_v1_1();
    }
    ASSERT_EQ(WidgetImpl_v1_2::numDestroyed(), 1);
}

}  // namespace
}  // namespace mongo::extension
