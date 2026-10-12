#include "ProjectionTypes.h"

#include <clusters/CameraAvStreamManagement/ClusterId.h>
#include <clusters/Descriptor/ClusterId.h>
#include <clusters/WebRTCTransportProvider/ClusterId.h>
#include <clusters/WebRTCTransportProvider/CommandIds.h>
#include <clusters/CameraAvStreamManagement/Enums.h>
#include <controller/CHIPControllerBuildConfig.h>
#include <clusters/shared/Enums.h>
#include <lib/core/CHIPError.h>

#if CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING
#include "CameraControllerRuntime.h"
#include <app/server-cluster/testing/FabricTestFixture.h>
#include <lib/support/TestPersistentStorageDelegate.h>
#include <system/SystemLayerImpl.h>
#endif

#include <iostream>
#include <stdexcept>
#include <filesystem>
#include <chrono>

namespace {
namespace Controller = winrt::Matter::Windows::Controller;
namespace Implementation = Controller::implementation;

void Require(bool condition, char const * message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void TestTypedDescriptors()
{
    auto first = winrt::make<Implementation::MatterDeviceType>(0x1234u, uint16_t(1));
    auto second = winrt::make<Implementation::MatterDeviceType>(0x5678u, uint16_t(2));
    auto camera = winrt::make<Implementation::MatterEndpointDescriptor>(
        uint16_t(2), std::vector<uint16_t>{ 3, 4 },
        std::vector<uint32_t>{ chip::app::Clusters::Descriptor::Id, chip::app::Clusters::CameraAvStreamManagement::Id,
                               chip::app::Clusters::WebRTCTransportProvider::Id },
        std::vector<uint32_t>{ chip::app::Clusters::Descriptor::Id },
        std::vector<Controller::MatterDeviceType>{ first, second });
    Require(camera.EndpointId() == 2, "Camera endpoint identity was lost");
    Require(camera.HasCameraClusters(), "Camera cluster composition was not recognized");
    auto parts = camera.PartsList();
    Require(parts.Size() == 2 && parts.GetAt(0) == 3 && parts.GetAt(1) == 4, "Typed numeric PartsList was lost");
    Require(camera.ServerClusters().Size() == 3, "Typed ServerList was lost");
    Require(camera.ClientClusters().GetAt(0) == chip::app::Clusters::Descriptor::Id, "Typed ClientList was lost");
    auto types = camera.DeviceTypes();
    Require(types.Size() == 2, "Typed structured DeviceTypeList was lost");
    Require(types.GetAt(0).DeviceTypeId() == 0x1234u && types.GetAt(0).Revision() == 1, "First device type was corrupted");
    Require(types.GetAt(1).DeviceTypeId() == 0x5678u && types.GetAt(1).Revision() == 2, "Second device type was corrupted");
    auto bridge = winrt::make<Implementation::MatterEndpointDescriptor>(
        uint16_t(1), std::vector<uint16_t>{ 2 }, std::vector<uint32_t>{ chip::app::Clusters::Descriptor::Id },
        std::vector<uint32_t>{}, std::vector<Controller::MatterDeviceType>{});
    Require(!bridge.HasCameraClusters(), "A bridge was incorrectly reported as a camera");
    Require(bridge.ClientClusters().Size() == 0 && bridge.DeviceTypes().Size() == 0, "Empty typed lists changed shape");
    auto empty = winrt::make<Implementation::MatterCameraDiscoveryResult>(
        winrt::single_threaded_vector<Controller::MatterEndpointDescriptor>().GetView());
    Require(empty.Succeeded() && empty.Cameras() && empty.Cameras().Size() == 0 && !empty.Failure(),
            "Reachable non-camera outcome was confused with failed discovery");
    auto found = winrt::make<Implementation::MatterCameraDiscoveryResult>(
        winrt::single_threaded_vector<Controller::MatterEndpointDescriptor>({ camera, camera }).GetView());
    Require(found.Succeeded() && found.Cameras().Size() == 2 && !found.Failure(), "Multiple discovered cameras were discarded");
    std::cout << "PASS typed numeric and structured descriptor collections; bridge and camera classification\n";
}

void TestControllerLifecycle();

#if CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING
class HostedEvents : public chip::app::DataModel::EventsGenerator
{
public:
    void ScheduleUrgentEventDeliverySync(std::optional<chip::FabricIndex>) override {}
    CHIP_ERROR GenerateEvent(chip::app::EventLoggingDelegate *, const chip::app::EventOptions &, chip::EventNumber &) override
    {
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }
    chip::System::Clock::Milliseconds64 GetMonotonicStartupTime() const override
    {
        return chip::System::Clock::Milliseconds64(0);
    }
};

class HostedExchange : public chip::app::DataModel::ActionContext
{
public:
    chip::Messaging::ExchangeContext * CurrentExchange() override { return nullptr; }
};

void TestHostedControllerComposition()
{
    using namespace chip;
    using namespace chip::app;
    Require(Platform::MemoryInit() == CHIP_NO_ERROR, "Hosted runtime memory initialization failed");
    auto memory = ScopeExit([] { Platform::MemoryShutdown(); });
    TestPersistentStorageDelegate storage;
    Testing::FabricTestFixture fabrics(&storage);
    FabricIndex fabric = 1;
    Require(fabrics.SetUpTestFabric(fabric) == CHIP_NO_ERROR, "Hosted runtime fabric initialization failed");
    auto fabricCleanup = ScopeExit([&] { Require(fabrics.TearDownTestFabric(fabric) == CHIP_NO_ERROR,
                                                "Hosted runtime fabric teardown failed"); });
    System::LayerImpl system;
    Require(system.Init() == CHIP_NO_ERROR, "Hosted runtime system initialization failed");
    auto systemCleanup = ScopeExit([&] { system.Shutdown(); });
    CASESessionManager sessions;
    HostedEvents events;
    HostedExchange exchange;
    DataModel::InteractionModelContext context{ events, exchange };
    Implementation::CameraControllerRuntime host(storage);
    Require(host.Init(storage) == CHIP_NO_ERROR, "Hosted runtime access initialization failed");
    auto hostCleanup = ScopeExit([&] {
        Require(host.Shutdown() == CHIP_NO_ERROR, "Hosted runtime shutdown failed");
        Require(host.Provider().Shutdown() == CHIP_NO_ERROR, "Hosted provider shutdown failed");
    });
    Require(host.Provider().Startup(context) == CHIP_NO_ERROR, "Hosted provider startup failed");
    storage.AddPoisonKey(DefaultStorageKeyAllocator::BindingTable().KeyName());
    uint64_t handle = 0;
    Require(host.Create(fabrics.GetFabricTable(), sessions, system, ScopedNodeId(0x1234, fabric), 2, 1, handle) ==
                CHIP_ERROR_PERSISTED_STORAGE_FAILED && host.ActiveHandle() == 0,
            "Binding initialization failure became a successful camera session");
    storage.ClearPoisonKeys();
    for (int i = 0; i < 2; ++i)
    {
        Require(host.Create(fabrics.GetFabricTable(), sessions, system, ScopedNodeId(0x1234, fabric), 2, 1, handle) ==
                    CHIP_NO_ERROR, "Hosted camera creation failed");
        ReadOnlyBufferBuilder<DataModel::ServerClusterEntry> builder;
        Require(host.Provider().ServerClusters(host.RequestorEndpoint(), builder) == CHIP_NO_ERROR,
                "Hosted metadata was not routable");
        const auto clusters = builder.TakeBuffer();
        Require(clusters.size() == 3, "Hosted camera endpoint did not include Binding");
        bool binding = false;
        for (const auto & cluster : clusters)
        {
            binding |= cluster.clusterId == Clusters::Binding::Id;
        }
        Require(binding, "Hosted camera endpoint did not use the shared Binding cluster");
        Access::SubjectDescriptor subject;
        subject.fabricIndex = fabric;
        subject.authMode = Access::AuthMode::kCase;
        subject.subject = 0x1234;
        Access::RequestPath path;
        path.endpoint = host.RequestorEndpoint();
        path.requestType = Access::RequestType::kAttributeReadRequest;
        for (const auto cluster : { Clusters::Descriptor::Id, Clusters::Binding::Id })
        {
            path.cluster = cluster;
            Require(Access::GetAccessControl().Check(subject, path, Access::Privilege::kView) == CHIP_NO_ERROR,
                    "Authenticated camera metadata read was denied");
            Require(Access::GetAccessControl().Check(subject, path, Access::Privilege::kAdminister) ==
                        CHIP_ERROR_ACCESS_DENIED, "Camera peer received Binding administration");
        }
        Require(host.CloseLocal(handle) == CHIP_NO_ERROR && host.ActiveHandle() == 0,
                "Hosted camera local cleanup failed");
    }
    std::cout << "PASS hosted shared Binding composition, initialization errors, peer metadata ACL, and repeated lifecycle\n";
}
#endif

void TestIceServers()
{
    auto urls = winrt::single_threaded_vector<winrt::hstring>({ L"stun:lan.invalid:3478", L"turns:relay.invalid:5349" });
    auto server = winrt::make<Implementation::MatterCameraIceServer>(urls.GetView(), true, L"", true, L"test-only-credential",
        winrt::box_value(uint16_t(0)).as<winrt::Windows::Foundation::IReference<uint16_t>>());
    urls.SetAt(0, L"stun:changed.invalid");
    Require(server.Urls().GetAt(0) == L"stun:lan.invalid:3478" && server.HasUsername() && server.Username().empty() &&
            server.HasCredential() && server.CertificateAuthorityId() && server.CertificateAuthorityId().Value() == 0,
            "ICE URLs were not owned or empty optional fields lost presence");
    auto absent = winrt::make<Implementation::MatterCameraIceServer>(urls.GetView(), false, L"", false, L"", nullptr);
    Require(!absent.HasUsername() && !absent.HasCredential() && !absent.CertificateAuthorityId(),
            "Absent ICE optional fields became present");
    for (auto const & url : { L"", L"https:relay.invalid", L"stun:host with space" })
    {
        bool rejected = false;
        try
        {
            winrt::make<Implementation::MatterCameraIceServer>(
                winrt::single_threaded_vector<winrt::hstring>({ url }).GetView(), false, L"", false, L"", nullptr);
        }
        catch (winrt::hresult_invalid_argument const &)
        {
            rejected = true;
        }
        Require(rejected, "Malformed ICE server URI was accepted");
    }
    std::cout << "PASS typed ICE server ownership and absent-versus-empty optional metadata\n";
}

void TestCameraCapabilities()
{
    namespace Commands = chip::app::Clusters::WebRTCTransportProvider::Commands;
    auto descriptor = winrt::make<Implementation::MatterEndpointDescriptor>(
        uint16_t(2), std::vector<uint16_t>{},
        std::vector<uint32_t>{ chip::app::Clusters::CameraAvStreamManagement::Id, chip::app::Clusters::WebRTCTransportProvider::Id },
        std::vector<uint32_t>{}, std::vector<Controller::MatterDeviceType>{});
    auto transport = winrt::make<Implementation::MatterCameraClusterCapabilities>(
        chip::app::Clusters::WebRTCTransportProvider::Id, uint16_t(1), uint32_t(0),
        std::vector<uint32_t>{ 0, 0xfffc },
        std::vector<uint32_t>{ Commands::ProvideOffer::Id, Commands::ProvideICECandidates::Id, Commands::EndSession::Id },
        std::vector<uint32_t>{ Commands::ProvideOfferResponse::Id });
    auto video = winrt::make<Implementation::MatterCameraClusterCapabilities>(
        chip::app::Clusters::CameraAvStreamManagement::Id, uint16_t(2),
        uint32_t(chip::app::Clusters::CameraAvStreamManagement::Feature::kVideo) | uint32_t(0x80000000),
        std::vector<uint32_t>{ 1, 2 }, std::vector<uint32_t>{}, std::vector<uint32_t>{});
    auto capabilities = winrt::make<Implementation::MatterCameraCapabilities>(descriptor, video, transport);
    Require(capabilities.SupportsVideo() && capabilities.SupportsClientOffer() &&
            capabilities.StreamManagement().FeatureMap() == 0x80000002 &&
            capabilities.StreamManagement().Revision() == 2 &&
            capabilities.TransportProvider().Attributes().Size() == 2,
            "Advertised features, revisions, or command metadata were lost");
    auto audioOnly = winrt::make<Implementation::MatterCameraClusterCapabilities>(
        chip::app::Clusters::CameraAvStreamManagement::Id, uint16_t(1), uint32_t(1),
        std::vector<uint32_t>{}, std::vector<uint32_t>{}, std::vector<uint32_t>{});
    Require(!winrt::make<Implementation::MatterCameraCapabilities>(descriptor, audioOnly, transport).SupportsClientOffer(),
            "An audio-only camera was accepted for video");
    for (size_t missing = 0; missing < 4; ++missing)
    {
        std::vector<uint32_t> accepted{ Commands::ProvideOffer::Id, Commands::ProvideICECandidates::Id, Commands::EndSession::Id };
        std::vector<uint32_t> generated{ Commands::ProvideOfferResponse::Id };
        if (missing == 3)
        {
            generated.clear();
        }
        else
        {
            accepted.erase(accepted.begin() + missing);
        }
        auto incomplete = winrt::make<Implementation::MatterCameraClusterCapabilities>(
            chip::app::Clusters::WebRTCTransportProvider::Id, uint16_t(1), uint32_t(0),
            std::vector<uint32_t>{}, accepted, generated);
        Require(!winrt::make<Implementation::MatterCameraCapabilities>(descriptor, video, incomplete).SupportsClientOffer(),
                "A camera missing a required command was accepted");
    }
    bool rejected = false;
    try
    {
        winrt::make<Implementation::MatterCameraClusterCapabilities>(uint32_t(1), uint16_t(0), uint32_t(0),
            std::vector<uint32_t>{}, std::vector<uint32_t>{}, std::vector<uint32_t>{});
    }
    catch (winrt::hresult_error const &)
    {
        rejected = true;
    }
    Require(rejected, "A zero cluster revision was accepted");
    std::cout << "PASS camera capabilities, unknown feature preservation, and required client-offer command gating\n";
}

void TestCameraValuesAndControllerLifecycle()
{
    auto candidate = winrt::make<Implementation::MatterCameraIceCandidate>(L"candidate:owned", L"video", 0);
    auto signal = winrt::make<Implementation::MatterCameraSignal>(uint64_t(5), Controller::MatterCameraSignalKind::Candidates,
        winrt::hstring{}, std::vector<Controller::MatterCameraIceCandidate>{ candidate }, uint32_t(0), uint16_t(42), uint8_t(2));
    Require(signal.SessionHandle() == 5 && signal.ProtocolSessionId() && signal.ProtocolSessionId().Value() == 42,
            "Local and protocol identities were lost");
    auto unknown = winrt::make<Implementation::MatterCameraSignal>(uint64_t(6), Controller::MatterCameraSignalKind::Failure,
        winrt::hstring{}, std::vector<Controller::MatterCameraIceCandidate>{}, CHIP_ERROR_TIMEOUT.AsInteger(),
        std::nullopt, uint8_t(0));
    Require(!unknown.ProtocolSessionId(), "An unbound failure invented a protocol session ID");
    auto zero = winrt::make<Implementation::MatterCameraSignal>(uint64_t(7), Controller::MatterCameraSignalKind::Failure,
        winrt::hstring{}, std::vector<Controller::MatterCameraIceCandidate>{}, CHIP_ERROR_TIMEOUT.AsInteger(),
        uint16_t(0), uint8_t(0));
    Require(zero.ProtocolSessionId() && zero.ProtocolSessionId().Value() == 0,
            "A valid protocol ID of zero lost its presence");
    Require(signal.Candidates().GetAt(0).LineIndex() == 0 && signal.Candidates().GetAt(0).Mid() == L"video",
            "Typed ICE metadata was lost");
    bool rejected = false;
    try
    {
        winrt::make<Implementation::MatterCameraIceCandidate>(L"candidate:bad", L"video", 65535);
    }

    catch (winrt::hresult_invalid_argument const &)
    {
        rejected = true;
    }
    Require(rejected, "Reserved nullable line index was accepted");
    TestControllerLifecycle();
}

void TestVideoStreamProjection()
    {
        namespace Collections = winrt::Windows::Foundation::Collections;
        Collections::PropertySet fields;
        fields.Insert(L"0", winrt::box_value(uint64_t(7)));
        fields.Insert(L"1", winrt::box_value(uint64_t(chip::app::Clusters::Globals::StreamUsageEnum::kLiveView)));
        fields.Insert(L"2", winrt::box_value(uint64_t(0)));
        fields.Insert(L"3", winrt::box_value(uint64_t(15)));
        fields.Insert(L"4", winrt::box_value(uint64_t(30)));
        Collections::PropertySet minimum;
        minimum.Insert(L"0", winrt::box_value(uint64_t(640)));
        minimum.Insert(L"1", winrt::box_value(uint64_t(480)));
        Collections::PropertySet maximum;
        maximum.Insert(L"0", winrt::box_value(uint64_t(1920)));
        maximum.Insert(L"1", winrt::box_value(uint64_t(1080)));
        fields.Insert(L"5", minimum);
        fields.Insert(L"6", maximum);
        fields.Insert(L"7", winrt::box_value(uint64_t(100000)));
        fields.Insert(L"8", winrt::box_value(uint64_t(4000000)));
        fields.Insert(L"9", winrt::box_value(uint64_t(2000)));
        fields.Insert(L"12", winrt::box_value(uint64_t(0)));
        auto stream = winrt::make<Implementation::MatterCameraVideoStream>(fields);
        Require(stream.StreamId() == 7 && stream.IsLiveView() && stream.VideoCodec() == 0, "Stream identity was corrupted");
        Require(stream.MinimumWidth() == 640 && stream.MaximumHeight() == 1080 && stream.MaximumFrameRate() == 30,
                "Video bounds were corrupted");
        Require(stream.MinimumBitRate() == 100000 && stream.KeyFrameInterval() == 2000, "Bitrate or key-frame metadata was lost");
        Require(!stream.WatermarkEnabled() && !stream.OnScreenDisplayEnabled(), "Absent optional fields became false");
        fields.Insert(L"10", winrt::box_value(false));
        fields.Insert(L"11", winrt::box_value(true));
        stream = winrt::make<Implementation::MatterCameraVideoStream>(fields);
        Require(stream.WatermarkEnabled() && !stream.WatermarkEnabled().Value() && stream.OnScreenDisplayEnabled().Value(),
                "Optional booleans lost presence or value");
        fields.Insert(L"2", winrt::box_value(uint64_t(255)));
        stream = winrt::make<Implementation::MatterCameraVideoStream>(fields);
        Require(stream.VideoCodec() == 255, "Unknown codec must remain explicit, not become a supported codec");
        auto expectRejected = [&]() {
            bool rejected = false;
            try
            {
                winrt::make<Implementation::MatterCameraVideoStream>(fields);
            }
            catch (winrt::hresult_error const &)
            {
                rejected = true;
            }
            Require(rejected, "Malformed allocated video metadata was accepted");
        };
        fields.Insert(L"0", winrt::box_value(uint64_t(65535)));
        expectRejected();
        fields.Insert(L"0", winrt::box_value(uint64_t(7)));
        fields.Insert(L"3", winrt::box_value(uint64_t(31)));
        expectRejected();
        fields.Insert(L"3", winrt::box_value(uint64_t(15)));
        fields.Remove(L"12");
        expectRejected();
        fields.Insert(L"12", winrt::box_value(uint64_t(256)));
        expectRejected();
        std::cout << "PASS typed allocated video stream bounds, unknown codec, optional booleans, and malformed metadata\n";
    }
void TestControllerLifecycle()
{
    bool rejected = false;
    std::vector<Controller::MatterControllerCamera> retainedClients;
    auto path = std::filesystem::temp_directory_path() /
        (L"matter-camera-projection-" + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
    for (int i = 0; i < 4; ++i)
    {
        auto options = winrt::make<Implementation::ControllerOptions>();
        constexpr winrt::guid publishedOptionsId{ 0xA743A4CB, 0x6683, 0x5F06,
                                                 { 0x91, 0x3E, 0xB0, 0xC1, 0x10, 0x40, 0xED, 0x9F } };
        Require(winrt::guid_of<Controller::IControllerOptions>() == publishedOptionsId,
                "Published controller options interface identity changed");
        auto existingOptions = options.as<Controller::IControllerOptions>();
        auto cameraOptions = options.as<Controller::IControllerOptions2>();
        Require(!options.EnableCameraSignaling(), "Camera signaling was enabled by default");
        existingOptions.StoragePath(winrt::hstring(path.wstring()));
        Require(options.StoragePath() == existingOptions.StoragePath(), "Existing options interface lost storage state");
        cameraOptions.EnableCameraSignaling(CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING && i % 2 != 0);
        Require(options.EnableCameraSignaling() == cameraOptions.EnableCameraSignaling(),
                "Versioned camera options interface lost opt-in state");
        auto controller = Implementation::MatterController::CreateAsync(options).get();
        auto camera = winrt::make<Implementation::MatterControllerCamera>(controller);
        auto discovery = winrt::make<Implementation::MatterControllerCameraDiscovery>(controller);
        auto invalidDiscovery = discovery.TryDiscoverCamerasAsync(0).get();
        Require(!invalidDiscovery.Succeeded() && !invalidDiscovery.Cameras() &&
                invalidDiscovery.Failure().Category() == Controller::MatterCameraFailureCategory::InvalidInput &&
                invalidDiscovery.Failure().Stage() == Controller::MatterCameraStage::Validation,
                "Discovery failure returned an empty success collection");
        auto invalidServers = camera.TryStartWithIceServersAsync(1, 2, 1, L"v=0\r\n", nullptr).get();
        Require(!invalidServers.Succeeded() && invalidServers.Failure().Stage() == Controller::MatterCameraStage::Validation &&
                invalidServers.Failure().Category() == Controller::MatterCameraFailureCategory::InvalidInput,
                "Invalid ICE server input did not produce a typed validation result");
        retainedClients.push_back(camera);
        Require(Implementation::MatterControllerCamera::IsSignalingSupported() ==
                (CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING != 0), "Compiled capability reporting is incorrect");
        auto invalid = camera.StartWithAllocatedVideoStreamAsync(0, 2, 1, L"v=0\r\n");
        rejected = false;
        try
        {
            invalid.get();
        }
        catch (winrt::hresult_invalid_argument const &)
        {
            rejected = true;
        }
        Require(rejected, "Invalid operational node was accepted");
        auto failed = camera.TryStartWithAllocatedVideoStreamAsync(0, 2, 1, L"v=0\r\n").get();
        Require(!failed.Succeeded() && !failed.Session() && failed.Failure(), "Failed start returned a success-shaped result");
        Require(failed.Failure().Stage() == Controller::MatterCameraStage::Validation &&
                failed.Failure().Category() == Controller::MatterCameraFailureCategory::InvalidInput &&
                !failed.Failure().HasNativeError() && !failed.Failure().Retryable(),
                "Invalid input did not preserve structured failure semantics");
        auto stale = winrt::make<Implementation::MatterCameraSession>(
            winrt::get_self<Implementation::MatterController>(controller)->Runtime(),
            UINT64_MAX, uint64_t(1), uint16_t(2), uint16_t(3));
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            try
            {
                stale.WaitForSignalAsync(1).get();
                Require(false, "A stale session handle was accepted");
            }
            catch (winrt::hresult_error const & error)
            {
                Require(error.message() != L"Only one signal consumer is allowed per session.",
                        "A failed wait retained the single-consumer guard");
            }
        }
#if CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING
        auto invalidCandidates = stale.TrySendCandidatesAsync(nullptr).get();
        Require(!invalidCandidates.Succeeded() &&
                invalidCandidates.Failure().Stage() == Controller::MatterCameraStage::Validation &&
                invalidCandidates.Failure().Category() == Controller::MatterCameraFailureCategory::InvalidInput,
                "Invalid candidates lost typed validation failure");
        auto staleCandidates = stale.TrySendCandidatesAsync(
            winrt::single_threaded_vector<Controller::MatterCameraIceCandidate>({
                winrt::make<Implementation::MatterCameraIceCandidate>(L"candidate:owned", L"video", 0)
            }).GetView()).get();
        Require(!staleCandidates.Succeeded() &&
                staleCandidates.Failure().Category() == Controller::MatterCameraFailureCategory::InvalidState &&
                staleCandidates.Failure().Stage() == Controller::MatterCameraStage::IceCandidates,
                "Candidate send lost native status or stage");
        if (options.EnableCameraSignaling())
        {
            Require(staleCandidates.Failure().HasNativeError() &&
                    staleCandidates.Failure().NativeError() == CHIP_ERROR_INCORRECT_STATE.AsInteger(),
                    "Native candidate failure lost its exact status");
        }
        auto ended = stale.TryCloseAsync().get();
        Require(ended.Succeeded() && !ended.Failure(), "Idempotent typed close reported a failure");
        Require(stale.GetStateAsync().get() == Controller::MatterCameraSessionState::Closed,
                "A released session did not report Closed");
        stale.CloseAsync().get();
        stale.CloseAsync().get();
#endif
        controller.CloseAsync().get();
        controller.CloseAsync().get();
#if CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING
        Require(stale.GetStateAsync().get() == Controller::MatterCameraSessionState::Closed,
                "Controller shutdown did not deterministically close its retained session");
#endif
        auto closedDiscovery = discovery.TryDiscoverCamerasAsync(1).get();
        Require(!closedDiscovery.Succeeded() && !closedDiscovery.Cameras() &&
                closedDiscovery.Failure().Stage() == Controller::MatterCameraStage::Discovery &&
                closedDiscovery.Failure().Category() == Controller::MatterCameraFailureCategory::InvalidState,
                "Closed controller discovery lost structured stage or failure semantics");
        auto closed = camera.TryStartWithAllocatedVideoStreamAsync(1, 2, 1, L"v=0\r\n").get();
        Require(!closed.Succeeded() && closed.Failure().Category() == Controller::MatterCameraFailureCategory::InvalidState,
                "A retained client accessed a closed controller");
    }
    // Only the generated test storage directory is removed.
    std::filesystem::remove_all(path);
    std::cout << "PASS typed ICE, optional controller initialization, stale-handle wait cleanup, close, and fabric reopening\n";
}
} // namespace

void TestStructuredCameraFailures()
{
    using Stage = Controller::MatterCameraStage;
    using Category = Controller::MatterCameraFailureCategory;
    struct Case
    {
        CHIP_ERROR native;
        Category category;
        bool retryable;
    };
    const Case cases[] = {
        { CHIP_ERROR_TIMEOUT, Category::Timeout, true },
        { CHIP_ERROR_BUSY, Category::ResourceExhausted, true },
        { CHIP_ERROR_ACCESS_DENIED, Category::AccessDenied, false },
        { CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE, Category::Unsupported, false },
        { CHIP_ERROR_NOT_CONNECTED, Category::ConnectionFailure, true },
        { CHIP_ERROR_INCORRECT_STATE, Category::InvalidState, false },
        { CHIP_ERROR_INVALID_ARGUMENT, Category::MalformedData, false },
        { CHIP_ERROR_INTERNAL, Category::Unknown, false }
    };
    for (auto const & test : cases)
    {
        auto failure = winrt::make<Implementation::MatterCameraFailure>(
            Stage::Signaling, int32_t(0x8007001f), true, test.native.AsInteger(), L"redacted stage diagnostic");
        Require(failure.HasNativeError() && failure.NativeError() == test.native.AsInteger(), "Native code was lost");
        Require(failure.Category() == test.category && failure.Retryable() == test.retryable, "Failure classification changed");
        Require(failure.HResult() == int32_t(0x8007001f) && failure.Stage() == Stage::Signaling, "Stage or HRESULT was lost");
    }
    auto privacy = winrt::make<Implementation::MatterCameraFailure>(
        Stage::Privacy, int32_t(0x80070005), false, uint32_t(0), L"Privacy mode enabled");
    Require(privacy.Category() == Category::PrivacyEnabled && !privacy.HasNativeError() && !privacy.Retryable(),
            "Privacy refusal was confused with native authorization failure");
    for (auto stage : { Stage::IceCandidates, Stage::SessionEnd })
    {
        auto failure = winrt::make<Implementation::MatterCameraFailure>(
            stage, int32_t(0x8007001f), true, CHIP_ERROR_TIMEOUT.AsInteger(), L"Bounded operation timed out");
        auto result = winrt::make<Implementation::MatterCameraOperationResult>(failure);
        Require(!result.Succeeded() && result.Failure().Stage() == stage && result.Failure().Retryable() &&
                result.Failure().NativeError() == CHIP_ERROR_TIMEOUT.AsInteger(), "Operation failure lost native diagnostics");
    }
    std::cout << "PASS structured camera failure categories, stage, HRESULT, native status, and retryability\n";
}

int main()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try
    {
        TestTypedDescriptors();
        TestIceServers();
        TestCameraCapabilities();
        TestVideoStreamProjection();
        TestStructuredCameraFailures();
#if CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING
        TestHostedControllerComposition();
#endif
        TestCameraValuesAndControllerLifecycle();
        bool rejected = false;
        try
        {
            winrt::make<Implementation::MatterControllerCameraDiscovery>(Controller::MatterController{ nullptr });
        }
        catch (winrt::hresult_invalid_argument const &)
        {
            rejected = true;
        }
        Require(rejected, "A null controller was accepted");
        std::cout << "PASS null controller validation\n";
    }
    catch (winrt::hresult_error const & error)
    {
        std::wcerr << L"FAIL HRESULT " << error.code() << L": " << error.message().c_str() << L'\n';
        return 1;
    }
    catch (std::exception const & error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
