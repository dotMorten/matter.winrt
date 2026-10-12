#pragma once

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include "AttributePath.g.h"
#include "AttributeReportEventArgs.g.h"
#include "AttributeSubscription.g.h"
#include "AttributeValue.g.h"
#include "BasicInformation.g.h"
#include "BasicInformationCluster.g.h"
#include "BleCommissioningParameters.g.h"
#include "BleNetworkCommissioningParameters.g.h"
#include "CommandPath.g.h"
#include "CommandResult.g.h"
#include "CommissionedNode.g.h"
#include "CommissioningProgressEventArgs.g.h"
#include "ControllerOptions.g.h"
#include "EventPath.g.h"
#include "EventReportEventArgs.g.h"
#include "EventSubscription.g.h"
#include "EventValue.g.h"
#include "LevelControlCluster.g.h"
#include "MatterController.g.h"
#include "MatterControllerCameraDiscovery.g.h"
#include "MatterControllerCamera.g.h"
#include "MatterCameraSession.g.h"
#include "MatterCameraSignal.g.h"
#include "MatterCameraIceCandidate.g.h"
#include "MatterCameraIceServer.g.h"
#include "MatterCameraVideoStream.g.h"
#include "MatterCameraVideoStreamOptions.g.h"
#include "MatterCameraVideoStreamAllocationResult.g.h"
#include "MatterCameraFailure.g.h"
#include "MatterCameraStartResult.g.h"
#include "MatterCameraOperationResult.g.h"
#include "MatterCameraDiscoveryResult.g.h"
#include "MatterCameraClusterCapabilities.g.h"
#include "MatterCameraCapabilities.g.h"
#include "MatterDeviceType.g.h"
#include "MatterEndpointDescriptor.g.h"
#include "MatterControllerRecovery.g.h"
#include "MatterControllerNetworkCommissioning.g.h"
#include "OnNetworkCommissioningParameters.g.h"
#include "OnOffCluster.g.h"
#include "TimedInteractionOptions.g.h"
#include "ThreadNetworkCredentials.g.h"
#include "WiFiNetworkCredentials.g.h"

#include <functional>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <clusters/CameraAvStreamManagement/Commands.h>

namespace winrt::Matter::Windows::Controller::implementation {

namespace Windows = ::winrt::Windows;

class ControllerRuntime;

struct ControllerOptions : ControllerOptionsT<ControllerOptions>
{
    hstring StoragePath() const;
    void StoragePath(hstring const & value);
    uint64_t ControllerNodeId() const;
    void ControllerNodeId(uint64_t value);
    uint32_t BluetoothAdapterId() const;
    void BluetoothAdapterId(uint32_t value);
    bool AllowTestAttestation() const;
    void AllowTestAttestation(bool value);
    bool EnableCameraSignaling() const { return mEnableCameraSignaling; }
    void EnableCameraSignaling(bool value) { mEnableCameraSignaling = value; }

private:
    hstring mStoragePath;
    uint64_t mControllerNodeId = 112233;
    uint32_t mBluetoothAdapterId = 0;
    bool mAllowTestAttestation = true;
    bool mEnableCameraSignaling = false;
};

struct OnNetworkCommissioningParameters : OnNetworkCommissioningParametersT<OnNetworkCommissioningParameters>
{
    uint64_t NodeId() const;
    void NodeId(uint64_t value);
    hstring SetupCode() const;
    void SetupCode(hstring const & value);
    uint32_t SetupPinCode() const;
    void SetupPinCode(uint32_t value);
    uint16_t LongDiscriminator() const;
    void LongDiscriminator(uint16_t value);
    hstring IpAddress() const;
    void IpAddress(hstring const & value);

private:
    uint64_t mNodeId = 1;
    hstring mSetupCode;
    uint32_t mSetupPinCode = 20202021;
    uint16_t mLongDiscriminator = 3840;
    hstring mIpAddress;
};

struct BleCommissioningParameters : BleCommissioningParametersT<BleCommissioningParameters>
{
    uint64_t NodeId() const;
    void NodeId(uint64_t value);
    uint32_t SetupPinCode() const;
    void SetupPinCode(uint32_t value);
    uint16_t LongDiscriminator() const;
    void LongDiscriminator(uint16_t value);

private:
    uint64_t mNodeId = 1;
    uint32_t mSetupPinCode = 20202021;
    uint16_t mLongDiscriminator = 3840;
};

struct WiFiNetworkCredentials : WiFiNetworkCredentialsT<WiFiNetworkCredentials>
{
    WiFiNetworkCredentials(hstring const & ssid, hstring const & passphrase);
    ~WiFiNetworkCredentials();
    uint32_t SsidLength() const;
    uint32_t PassphraseLength() const;
    std::vector<uint8_t> const & Ssid() const;
    std::vector<uint8_t> const & Passphrase() const;

private:
    std::vector<uint8_t> mSsid;
    std::vector<uint8_t> mPassphrase;
};

struct ThreadNetworkCredentials : ThreadNetworkCredentialsT<ThreadNetworkCredentials>
{
    explicit ThreadNetworkCredentials(Windows::Storage::Streams::IBuffer const & operationalDataset);
    ~ThreadNetworkCredentials();
    uint32_t DatasetLength() const;
    std::vector<uint8_t> const & OperationalDataset() const;

private:
    std::vector<uint8_t> mOperationalDataset;
};

struct BleNetworkCommissioningParameters : BleNetworkCommissioningParametersT<BleNetworkCommissioningParameters>
{
    uint64_t NodeId() const;
    void NodeId(uint64_t value);
    uint32_t SetupPinCode() const;
    void SetupPinCode(uint32_t value);
    uint16_t LongDiscriminator() const;
    void LongDiscriminator(uint16_t value);
    Controller::WiFiNetworkCredentials WiFi() const;
    void WiFi(Controller::WiFiNetworkCredentials const & value);
    Controller::ThreadNetworkCredentials Thread() const;
    void Thread(Controller::ThreadNetworkCredentials const & value);

private:
    uint64_t mNodeId = 1;
    uint32_t mSetupPinCode = 20202021;
    uint16_t mLongDiscriminator = 3840;
    Controller::WiFiNetworkCredentials mWiFi{ nullptr };
    Controller::ThreadNetworkCredentials mThread{ nullptr };
};

struct AttributePath : AttributePathT<AttributePath>
{
    AttributePath(uint16_t endpointId, uint32_t clusterId, uint32_t attributeId);
    uint16_t EndpointId() const;
    uint32_t ClusterId() const;
    uint32_t AttributeId() const;

private:
    uint16_t mEndpointId;
    uint32_t mClusterId;
    uint32_t mAttributeId;
};

struct CommandPath : CommandPathT<CommandPath>
{
    CommandPath(uint16_t endpointId, uint32_t clusterId, uint32_t commandId);
    uint16_t EndpointId() const;
    uint32_t ClusterId() const;
    uint32_t CommandId() const;

private:
    uint16_t mEndpointId;
    uint32_t mClusterId;
    uint32_t mCommandId;
};

struct EventPath : EventPathT<EventPath>
{
    EventPath(uint16_t endpointId, uint32_t clusterId, uint32_t eventId, bool urgent);
    uint16_t EndpointId() const;
    uint32_t ClusterId() const;
    uint32_t EventId() const;
    bool Urgent() const;

private:
    uint16_t mEndpointId;
    uint32_t mClusterId;
    uint32_t mEventId;
    bool mUrgent;
};

struct CommissionedNode : CommissionedNodeT<CommissionedNode>
{
    CommissionedNode(uint64_t nodeId, uint16_t fabricIndex);
    uint64_t NodeId() const;
    uint16_t FabricIndex() const;

private:
    uint64_t mNodeId;
    uint16_t mFabricIndex;
};

struct CommissioningProgressEventArgs : CommissioningProgressEventArgsT<CommissioningProgressEventArgs>
{
    CommissioningProgressEventArgs(Controller::CommissioningStage stage, hstring message);
    Controller::CommissioningStage Stage() const;
    hstring Message() const;

private:
    Controller::CommissioningStage mStage;
    hstring mMessage;
};

struct AttributeValue : AttributeValueT<AttributeValue>
{
    AttributeValue(Controller::AttributePath path, Windows::Foundation::Collections::IPropertySet data);
    Controller::AttributePath Path() const;
    Windows::Foundation::Collections::IPropertySet Data() const;

private:
    Controller::AttributePath mPath;
    Windows::Foundation::Collections::IPropertySet mData;
};

struct CommandResult : CommandResultT<CommandResult>
{
    CommandResult(Controller::CommandPath path, Windows::Foundation::Collections::IPropertySet data);
    Controller::CommandPath Path() const;
    Windows::Foundation::Collections::IPropertySet Data() const;

private:
    Controller::CommandPath mPath;
    Windows::Foundation::Collections::IPropertySet mData;
};

struct TimedInteractionOptions : TimedInteractionOptionsT<TimedInteractionOptions>
{
    explicit TimedInteractionOptions(uint16_t timeoutMilliseconds);
    uint16_t TimeoutMilliseconds() const;

private:
    uint16_t mTimeoutMilliseconds;
};

struct EventValue : EventValueT<EventValue>
{
    EventValue(Controller::EventPath path, uint64_t eventNumber, Windows::Foundation::Collections::IPropertySet data);
    Controller::EventPath Path() const;
    uint64_t EventNumber() const;
    Windows::Foundation::Collections::IPropertySet Data() const;

private:
    Controller::EventPath mPath;
    uint64_t mEventNumber;
    Windows::Foundation::Collections::IPropertySet mData;
};

struct EventReportEventArgs : EventReportEventArgsT<EventReportEventArgs>
{
    explicit EventReportEventArgs(Controller::EventValue value);
    Controller::EventValue Value() const;

private:
    Controller::EventValue mValue;
};

struct EventSubscription : EventSubscriptionT<EventSubscription>
{
    EventSubscription() = default;
    explicit EventSubscription(std::function<void()> close);
    event_token ReportReceived(
        Windows::Foundation::TypedEventHandler<Controller::EventSubscription, Controller::EventReportEventArgs> const & handler);
    void ReportReceived(event_token const & token) noexcept;
    Windows::Foundation::IAsyncAction CloseAsync();
    void Publish(Controller::EventReportEventArgs const & args);

private:
    winrt::event<Windows::Foundation::TypedEventHandler<Controller::EventSubscription, Controller::EventReportEventArgs>>
        mReportReceived;
    std::mutex mReportMutex;
    std::vector<Controller::EventReportEventArgs> mPendingReports;
    size_t mReportHandlerCount = 0;
    bool mDrainingPendingReports = false;
    std::function<void()> mClose;
};

struct AttributeReportEventArgs : AttributeReportEventArgsT<AttributeReportEventArgs>
{
    explicit AttributeReportEventArgs(Controller::AttributeValue value);
    Controller::AttributeValue Value() const;

private:
    Controller::AttributeValue mValue;
};

struct AttributeSubscription : AttributeSubscriptionT<AttributeSubscription>
{
    AttributeSubscription() = default;
    explicit AttributeSubscription(std::function<void()> close);
    event_token ReportReceived(
        Windows::Foundation::TypedEventHandler<Controller::AttributeSubscription, Controller::AttributeReportEventArgs> const & handler);
    void ReportReceived(event_token const & token) noexcept;
    Windows::Foundation::IAsyncAction CloseAsync();
    void Publish(Controller::AttributeReportEventArgs const & args);

private:
    winrt::event<Windows::Foundation::TypedEventHandler<Controller::AttributeSubscription, Controller::AttributeReportEventArgs>>
        mReportReceived;
    std::mutex mReportMutex;
    std::vector<Controller::AttributeReportEventArgs> mPendingReports;
    size_t mReportHandlerCount = 0;
    bool mDrainingPendingReports = false;
    std::function<void()> mClose;
};

struct BasicInformation : BasicInformationT<BasicInformation>
{
    BasicInformation(uint16_t vendorId, hstring vendorName, uint16_t productId, hstring productName, hstring nodeLabel,
                     hstring serialNumber, uint32_t softwareVersion, hstring softwareVersionString);
    uint16_t VendorId() const;
    hstring VendorName() const;
    uint16_t ProductId() const;
    hstring ProductName() const;
    hstring NodeLabel() const;
    hstring SerialNumber() const;
    uint32_t SoftwareVersion() const;
    hstring SoftwareVersionString() const;

private:
    uint16_t mVendorId;
    hstring mVendorName;
    uint16_t mProductId;
    hstring mProductName;
    hstring mNodeLabel;
    hstring mSerialNumber;
    uint32_t mSoftwareVersion;
    hstring mSoftwareVersionString;
};

struct OnOffCluster : OnOffClusterT<OnOffCluster>
{
    OnOffCluster(std::shared_ptr<ControllerRuntime> runtime, uint64_t nodeId, uint16_t endpointId);
    Windows::Foundation::IAsyncOperation<bool> ReadAsync();
    Windows::Foundation::IAsyncAction SetAsync(bool value);
    Windows::Foundation::IAsyncAction OnAsync();
    Windows::Foundation::IAsyncAction OffAsync();
    Windows::Foundation::IAsyncAction ToggleAsync();
    Windows::Foundation::IAsyncOperation<Controller::AttributeSubscription> SubscribeAsync(uint16_t minimumIntervalSeconds,
                                                                                           uint16_t maximumIntervalSeconds);

private:
    std::shared_ptr<ControllerRuntime> mRuntime;
    uint64_t mNodeId;
    uint16_t mEndpointId;
};

struct LevelControlCluster : LevelControlClusterT<LevelControlCluster>
{
    LevelControlCluster(std::shared_ptr<ControllerRuntime> runtime, uint64_t nodeId, uint16_t endpointId);
    Windows::Foundation::IAsyncOperation<uint8_t> ReadCurrentLevelAsync();
    Windows::Foundation::IAsyncAction MoveToLevelAsync(uint8_t level, uint16_t transitionTime, uint8_t optionsMask,
                                                       uint8_t optionsOverride);
    Windows::Foundation::IAsyncOperation<Controller::AttributeSubscription> SubscribeAsync(uint16_t minimumIntervalSeconds,
                                                                                           uint16_t maximumIntervalSeconds);

private:
    std::shared_ptr<ControllerRuntime> mRuntime;
    uint64_t mNodeId;
    uint16_t mEndpointId;
};

struct BasicInformationCluster : BasicInformationClusterT<BasicInformationCluster>
{
    BasicInformationCluster(std::shared_ptr<ControllerRuntime> runtime, uint64_t nodeId, uint16_t endpointId);
    Windows::Foundation::IAsyncOperation<Controller::BasicInformation> ReadAsync();

private:
    std::shared_ptr<ControllerRuntime> mRuntime;
    uint64_t mNodeId;
    uint16_t mEndpointId;
};

struct MatterController : MatterControllerT<MatterController>
{
    explicit MatterController(std::shared_ptr<ControllerRuntime> runtime);
    static Windows::Foundation::IAsyncOperation<Controller::MatterController> CreateAsync(Controller::ControllerOptions options);
    event_token CommissioningProgress(
        Windows::Foundation::TypedEventHandler<Controller::MatterController, Controller::CommissioningProgressEventArgs> const & handler);
    void CommissioningProgress(event_token const & token) noexcept;
    Windows::Foundation::IAsyncOperation<Controller::CommissionedNode>
    CommissionOnNetworkAsync(Controller::OnNetworkCommissioningParameters parameters);
    Windows::Foundation::IAsyncOperation<Controller::CommissionedNode>
    CommissionBleAsync(Controller::BleCommissioningParameters parameters);
    Windows::Foundation::Collections::IVectorView<Controller::CommissionedNode> CommissionedNodes();
    Windows::Foundation::IAsyncAction RemoveNodeAsync(uint64_t nodeId);
    Windows::Foundation::IAsyncOperation<Controller::AttributeValue> ReadAttributeAsync(uint64_t nodeId,
                                                                                        Controller::AttributePath path);
    Windows::Foundation::IAsyncAction WriteAttributeAsync(uint64_t nodeId, Controller::AttributePath path,
                                                          Windows::Foundation::Collections::IPropertySet value);
    Windows::Foundation::IAsyncAction WriteAttributeTimedAsync(
        uint64_t nodeId, Controller::AttributePath path, Windows::Foundation::Collections::IPropertySet value,
        Controller::TimedInteractionOptions options);
    Windows::Foundation::IAsyncOperation<Controller::CommandResult>
    InvokeCommandAsync(uint64_t nodeId, Controller::CommandPath path, Windows::Foundation::Collections::IPropertySet arguments);
    Windows::Foundation::IAsyncOperation<Controller::CommandResult> InvokeCommandTimedAsync(
        uint64_t nodeId, Controller::CommandPath path, Windows::Foundation::Collections::IPropertySet arguments,
        Controller::TimedInteractionOptions options);
    Windows::Foundation::IAsyncOperation<Controller::AttributeSubscription>
    SubscribeAttributeAsync(uint64_t nodeId, Controller::AttributePath path, uint16_t minimumIntervalSeconds,
                            uint16_t maximumIntervalSeconds);
    Windows::Foundation::IAsyncOperation<Windows::Foundation::Collections::IVectorView<Controller::EventValue>>
    ReadEventsAsync(uint64_t nodeId, Controller::EventPath path, uint64_t minimumEventNumber);
    Windows::Foundation::IAsyncOperation<Controller::EventSubscription>
    SubscribeEventAsync(uint64_t nodeId, Controller::EventPath path, uint64_t minimumEventNumber,
                        uint16_t minimumIntervalSeconds, uint16_t maximumIntervalSeconds);
    Controller::OnOffCluster GetOnOffCluster(uint64_t nodeId, uint16_t endpointId);
    Controller::LevelControlCluster GetLevelControlCluster(uint64_t nodeId, uint16_t endpointId);
    Controller::BasicInformationCluster GetBasicInformationCluster(uint64_t nodeId, uint16_t endpointId);
    Windows::Foundation::IAsyncAction CloseAsync();
    std::shared_ptr<ControllerRuntime> Runtime();

private:
    std::mutex mRuntimeMutex;
    std::shared_ptr<ControllerRuntime> mRuntime;
    winrt::event<Windows::Foundation::TypedEventHandler<Controller::MatterController, Controller::CommissioningProgressEventArgs>>
        mCommissioningProgress;
};

struct MatterCameraIceCandidate : MatterCameraIceCandidateT<MatterCameraIceCandidate>
{
    MatterCameraIceCandidate(hstring candidate, hstring mid, int32_t lineIndex);
    hstring Candidate() const { return mCandidate; }
    hstring Mid() const { return mMid; }
    int32_t LineIndex() const { return mLineIndex; }
private:
    hstring mCandidate;
    hstring mMid;
    int32_t mLineIndex;
};

struct MatterCameraSignal : MatterCameraSignalT<MatterCameraSignal>
{
    MatterCameraSignal(uint64_t handle, Controller::MatterCameraSignalKind kind, hstring sdp,
                      std::vector<Controller::MatterCameraIceCandidate> candidates, uint32_t error,
                      std::optional<uint16_t> protocolId, uint8_t reason);
    uint64_t SessionHandle() const { return mHandle; }
    Controller::MatterCameraSignalKind Kind() const { return mKind; }
    hstring Sdp() const { return mSdp; }
    Windows::Foundation::Collections::IVectorView<Controller::MatterCameraIceCandidate> Candidates() const { return mCandidates; }
    uint32_t NativeError() const { return mError; }
    Windows::Foundation::IReference<uint16_t> ProtocolSessionId() const { return mProtocolId; }
    uint8_t EndReason() const { return mReason; }
private:
    uint64_t mHandle;
    Controller::MatterCameraSignalKind mKind;
    hstring mSdp;
    Windows::Foundation::Collections::IVectorView<Controller::MatterCameraIceCandidate> mCandidates;
    uint32_t mError;
    Windows::Foundation::IReference<uint16_t> mProtocolId{ nullptr };
    uint8_t mReason;
};

struct MatterCameraSession : MatterCameraSessionT<MatterCameraSession>
{
    MatterCameraSession(std::shared_ptr<ControllerRuntime> runtime, uint64_t handle, uint64_t node,
                       uint16_t endpoint, uint16_t requestor);
    uint64_t Handle() const { return mHandle; }
    uint64_t NodeId() const { return mNode; }
    uint16_t CameraEndpointId() const { return mEndpoint; }
    uint16_t RequestorEndpointId() const { return mRequestor; }
    Windows::Foundation::IAsyncOperation<Controller::MatterCameraSessionState> GetStateAsync();
    Windows::Foundation::IAsyncOperation<Controller::MatterCameraSignal> WaitForSignalAsync(uint32_t timeoutMilliseconds);
    Windows::Foundation::IAsyncAction SendCandidatesAsync(
        Windows::Foundation::Collections::IVectorView<Controller::MatterCameraIceCandidate> candidates);
    Windows::Foundation::IAsyncAction CloseAsync();
    Windows::Foundation::IAsyncOperation<Controller::MatterCameraOperationResult> TrySendCandidatesAsync(
        Windows::Foundation::Collections::IVectorView<Controller::MatterCameraIceCandidate> candidates);
    Windows::Foundation::IAsyncOperation<Controller::MatterCameraOperationResult> TryCloseAsync();
private:
    std::shared_ptr<ControllerRuntime> mRuntime;
    uint64_t mHandle;
    uint64_t mNode;
    uint16_t mEndpoint;
    uint16_t mRequestor;
    std::atomic_bool mWaiting{ false };
    std::atomic_uint32_t mClosing{ 0 };
};

struct MatterCameraIceServer : MatterCameraIceServerT<MatterCameraIceServer>
{
    MatterCameraIceServer(Windows::Foundation::Collections::IVectorView<hstring> urls,
        bool hasUsername, hstring username, bool hasCredential, hstring credential,
        Windows::Foundation::IReference<uint16_t> certificateAuthorityId);
    Windows::Foundation::Collections::IVectorView<hstring> Urls() const { return mUrls; }
    bool HasUsername() const { return mHasUsername; }
    hstring Username() const { return mUsername; }
    bool HasCredential() const { return mHasCredential; }
    hstring Credential() const { return mCredential; }
    Windows::Foundation::IReference<uint16_t> CertificateAuthorityId() const { return mCertificateAuthorityId; }
private:
    Windows::Foundation::Collections::IVectorView<hstring> mUrls;
    bool mHasUsername;
    hstring mUsername;
    bool mHasCredential;
    hstring mCredential;
    Windows::Foundation::IReference<uint16_t> mCertificateAuthorityId{ nullptr };
};

struct MatterControllerCamera : MatterControllerCameraT<MatterControllerCamera>
{
    explicit MatterControllerCamera(Controller::MatterController controller);
    static bool IsSignalingSupported();
    Windows::Foundation::IAsyncOperation<Controller::MatterCameraCapabilities> ReadCapabilitiesAsync(
        uint64_t nodeId, uint16_t endpointId);
    Windows::Foundation::IAsyncOperation<Windows::Foundation::Collections::IVectorView<Controller::MatterCameraVideoStream>>
    ReadAllocatedVideoStreamsAsync(uint64_t nodeId, uint16_t endpointId);
    Windows::Foundation::IAsyncOperation<Controller::MatterCameraSession> StartWithAllocatedVideoStreamAsync(
        uint64_t nodeId, uint16_t endpointId, uint16_t videoStreamId, hstring offerSdp);
    Windows::Foundation::IAsyncOperation<Controller::MatterCameraStartResult> TryStartWithAllocatedVideoStreamAsync(
        uint64_t nodeId, uint16_t endpointId, uint16_t videoStreamId, hstring offerSdp);
    Windows::Foundation::IAsyncOperation<Controller::MatterCameraStartResult> TryStartWithIceServersAsync(
        uint64_t nodeId, uint16_t endpointId, uint16_t videoStreamId, hstring offerSdp,
        Windows::Foundation::Collections::IVectorView<Controller::MatterCameraIceServer> iceServers);
    Windows::Foundation::IAsyncOperation<Controller::MatterCameraVideoStream> AllocateVideoStreamAsync(
        uint64_t nodeId, uint16_t endpointId, Controller::MatterCameraVideoStreamOptions options);
    Windows::Foundation::IAsyncOperation<bool> IsPrivacyEnabledAsync(uint64_t nodeId, uint16_t endpointId);
    Windows::Foundation::IAsyncOperation<Controller::MatterCameraVideoStreamAllocationResult> TryAllocateVideoStreamAsync(
        uint64_t nodeId, uint16_t endpointId, Controller::MatterCameraVideoStreamOptions options);
    Windows::Foundation::IAsyncAction DeallocateVideoStreamAsync(uint64_t nodeId, uint16_t endpointId, uint16_t videoStreamId);
    Windows::Foundation::IAsyncOperation<Controller::MatterCameraOperationResult> TryDeallocateVideoStreamAsync(
        uint64_t nodeId, uint16_t endpointId, uint16_t videoStreamId);
private:
    std::shared_ptr<ControllerRuntime> mRuntime;
};

struct MatterCameraVideoStreamOptions : MatterCameraVideoStreamOptionsT<MatterCameraVideoStreamOptions>
{
    MatterCameraVideoStreamOptions();
    uint16_t MinimumWidth() const { return mRequest.minResolution.width; }
    void MinimumWidth(uint16_t value) { mRequest.minResolution.width = value; }
    uint16_t MaximumWidth() const { return mRequest.maxResolution.width; }
    void MaximumWidth(uint16_t value) { mRequest.maxResolution.width = value; }
    uint16_t MinimumHeight() const { return mRequest.minResolution.height; }
    void MinimumHeight(uint16_t value) { mRequest.minResolution.height = value; }
    uint16_t MaximumHeight() const { return mRequest.maxResolution.height; }
    void MaximumHeight(uint16_t value) { mRequest.maxResolution.height = value; }
    uint16_t MinimumFrameRate() const { return mRequest.minFrameRate; }
    void MinimumFrameRate(uint16_t value) { mRequest.minFrameRate = value; }
    uint16_t MaximumFrameRate() const { return mRequest.maxFrameRate; }
    void MaximumFrameRate(uint16_t value) { mRequest.maxFrameRate = value; }
    uint32_t MinimumBitRate() const { return mRequest.minBitRate; }
    void MinimumBitRate(uint32_t value) { mRequest.minBitRate = value; }
    uint32_t MaximumBitRate() const { return mRequest.maxBitRate; }
    void MaximumBitRate(uint32_t value) { mRequest.maxBitRate = value; }
    uint16_t KeyFrameInterval() const { return mRequest.keyFrameInterval; }
    void KeyFrameInterval(uint16_t value) { mRequest.keyFrameInterval = value; }
    chip::app::Clusters::CameraAvStreamManagement::Commands::VideoStreamAllocate::Type Snapshot() const;
    static void ValidateAllocated(
        chip::app::Clusters::CameraAvStreamManagement::Commands::VideoStreamAllocate::Type const & request,
        Controller::MatterCameraVideoStream const & stream);
private:
    chip::app::Clusters::CameraAvStreamManagement::Commands::VideoStreamAllocate::Type mRequest;
};

struct MatterCameraVideoStreamAllocationResult :
    MatterCameraVideoStreamAllocationResultT<MatterCameraVideoStreamAllocationResult>
{
    explicit MatterCameraVideoStreamAllocationResult(Controller::MatterCameraVideoStream stream) : mStream(std::move(stream)) {}
    explicit MatterCameraVideoStreamAllocationResult(Controller::MatterCameraFailure failure) : mFailure(std::move(failure)) {}
    bool Succeeded() const { return mStream != nullptr; }
    Controller::MatterCameraVideoStream Stream() const { return mStream; }
    Controller::MatterCameraFailure Failure() const { return mFailure; }
private:
    Controller::MatterCameraVideoStream mStream{ nullptr };
    Controller::MatterCameraFailure mFailure{ nullptr };
};

struct MatterCameraFailure : MatterCameraFailureT<MatterCameraFailure>
{
    MatterCameraFailure(Controller::MatterCameraStage stage, int32_t result, bool hasNativeError,
                       uint32_t nativeError, hstring message);
    Controller::MatterCameraStage Stage() const { return mStage; }
    Controller::MatterCameraFailureCategory Category() const { return mCategory; }
    int32_t HResult() const { return mHResult; }
    bool HasNativeError() const { return mHasNativeError; }
    uint32_t NativeError() const { return mNativeError; }
    bool Retryable() const;
    hstring Message() const { return mMessage; }
private:
    Controller::MatterCameraStage mStage;
    Controller::MatterCameraFailureCategory mCategory;
    int32_t mHResult;
    bool mHasNativeError;
    uint32_t mNativeError;
    hstring mMessage;
};

struct MatterCameraStartResult : MatterCameraStartResultT<MatterCameraStartResult>
{
    explicit MatterCameraStartResult(Controller::MatterCameraSession session) : mSession(std::move(session)) {}
    explicit MatterCameraStartResult(Controller::MatterCameraFailure failure) : mFailure(std::move(failure)) {}
    bool Succeeded() const { return mSession != nullptr; }
    Controller::MatterCameraSession Session() const { return mSession; }
    Controller::MatterCameraFailure Failure() const { return mFailure; }
private:
    Controller::MatterCameraSession mSession{ nullptr };
    Controller::MatterCameraFailure mFailure{ nullptr };
};

struct MatterCameraOperationResult : MatterCameraOperationResultT<MatterCameraOperationResult>
{
    MatterCameraOperationResult() = default;
    explicit MatterCameraOperationResult(Controller::MatterCameraFailure failure) : mFailure(std::move(failure)) {}
    bool Succeeded() const { return mFailure == nullptr; }
    Controller::MatterCameraFailure Failure() const { return mFailure; }
private:
    Controller::MatterCameraFailure mFailure{ nullptr };
};

struct MatterCameraVideoStream : MatterCameraVideoStreamT<MatterCameraVideoStream>
{
    explicit MatterCameraVideoStream(Windows::Foundation::Collections::IPropertySet const & fields);
    uint16_t StreamId() const { return mStreamId; }
    uint8_t StreamUsage() const { return mStreamUsage; }
    bool IsLiveView() const;
    uint8_t VideoCodec() const { return mVideoCodec; }
    uint16_t MinimumFrameRate() const { return mMinimumFrameRate; }
    uint16_t MaximumFrameRate() const { return mMaximumFrameRate; }
    uint16_t MinimumWidth() const { return mMinimumWidth; }
    uint16_t MinimumHeight() const { return mMinimumHeight; }
    uint16_t MaximumWidth() const { return mMaximumWidth; }
    uint16_t MaximumHeight() const { return mMaximumHeight; }
    uint32_t MinimumBitRate() const { return mMinimumBitRate; }
    uint32_t MaximumBitRate() const { return mMaximumBitRate; }
    uint16_t KeyFrameInterval() const { return mKeyFrameInterval; }
    uint8_t ReferenceCount() const { return mReferenceCount; }
    Windows::Foundation::IReference<bool> WatermarkEnabled() const { return mWatermark; }
    Windows::Foundation::IReference<bool> OnScreenDisplayEnabled() const { return mOnScreenDisplay; }
private:
    uint16_t mStreamId;
    uint8_t mStreamUsage;
    uint8_t mVideoCodec;
    uint16_t mMinimumFrameRate;
    uint16_t mMaximumFrameRate;
    uint16_t mMinimumWidth;
    uint16_t mMinimumHeight;
    uint16_t mMaximumWidth;
    uint16_t mMaximumHeight;
    uint32_t mMinimumBitRate;
    uint32_t mMaximumBitRate;
    uint16_t mKeyFrameInterval;
    uint8_t mReferenceCount;
    Windows::Foundation::IReference<bool> mWatermark{ nullptr };
    Windows::Foundation::IReference<bool> mOnScreenDisplay{ nullptr };
};

struct MatterCameraClusterCapabilities : MatterCameraClusterCapabilitiesT<MatterCameraClusterCapabilities>
{
    MatterCameraClusterCapabilities(uint32_t cluster, uint16_t revision, uint32_t features, std::vector<uint32_t> attributes,
        std::vector<uint32_t> accepted, std::vector<uint32_t> generated);
    uint32_t ClusterId() const { return mCluster; }
    uint16_t Revision() const { return mRevision; }
    uint32_t FeatureMap() const { return mFeatures; }
    Windows::Foundation::Collections::IVectorView<uint32_t> Attributes() const { return mAttributes; }
    Windows::Foundation::Collections::IVectorView<uint32_t> AcceptedCommands() const { return mAccepted; }
    Windows::Foundation::Collections::IVectorView<uint32_t> GeneratedCommands() const { return mGenerated; }
private:
    uint32_t mCluster;
    uint16_t mRevision;
    uint32_t mFeatures;
    Windows::Foundation::Collections::IVectorView<uint32_t> mAttributes;
    Windows::Foundation::Collections::IVectorView<uint32_t> mAccepted;
    Windows::Foundation::Collections::IVectorView<uint32_t> mGenerated;
};

struct MatterCameraCapabilities : MatterCameraCapabilitiesT<MatterCameraCapabilities>
{
    MatterCameraCapabilities(Controller::MatterEndpointDescriptor descriptor,
        Controller::MatterCameraClusterCapabilities stream, Controller::MatterCameraClusterCapabilities transport) :
        mDescriptor(std::move(descriptor)), mStream(std::move(stream)), mTransport(std::move(transport)) {}
    Controller::MatterEndpointDescriptor Descriptor() const { return mDescriptor; }
    Controller::MatterCameraClusterCapabilities StreamManagement() const { return mStream; }
    Controller::MatterCameraClusterCapabilities TransportProvider() const { return mTransport; }
    bool SupportsVideo() const;
    bool SupportsClientOffer() const;
private:
    Controller::MatterEndpointDescriptor mDescriptor;
    Controller::MatterCameraClusterCapabilities mStream;
    Controller::MatterCameraClusterCapabilities mTransport;
};

struct MatterDeviceType : MatterDeviceTypeT<MatterDeviceType>
{
    MatterDeviceType(uint32_t deviceTypeId, uint16_t revision) : mDeviceTypeId(deviceTypeId), mRevision(revision) {}
    uint32_t DeviceTypeId() const { return mDeviceTypeId; }
    uint16_t Revision() const { return mRevision; }

private:
    uint32_t mDeviceTypeId;
    uint16_t mRevision;
};

struct MatterEndpointDescriptor : MatterEndpointDescriptorT<MatterEndpointDescriptor>
{
    MatterEndpointDescriptor(uint16_t endpointId, std::vector<uint16_t> parts, std::vector<uint32_t> servers,
                             std::vector<uint32_t> clients, std::vector<Controller::MatterDeviceType> types);
    uint16_t EndpointId() const { return mEndpointId; }
    Windows::Foundation::Collections::IVectorView<uint16_t> PartsList() const { return mParts; }
    Windows::Foundation::Collections::IVectorView<uint32_t> ServerClusters() const { return mServers; }
    Windows::Foundation::Collections::IVectorView<uint32_t> ClientClusters() const { return mClients; }
    Windows::Foundation::Collections::IVectorView<Controller::MatterDeviceType> DeviceTypes() const { return mTypes; }
    bool HasCameraClusters() const;

private:
    uint16_t mEndpointId;
    Windows::Foundation::Collections::IVectorView<uint16_t> mParts;
    Windows::Foundation::Collections::IVectorView<uint32_t> mServers;
    Windows::Foundation::Collections::IVectorView<uint32_t> mClients;
    Windows::Foundation::Collections::IVectorView<Controller::MatterDeviceType> mTypes;
};

struct MatterControllerCameraDiscovery : MatterControllerCameraDiscoveryT<MatterControllerCameraDiscovery>
{
    explicit MatterControllerCameraDiscovery(Controller::MatterController controller);
    Windows::Foundation::IAsyncOperation<Controller::MatterEndpointDescriptor> ReadDescriptorAsync(uint64_t nodeId,
                                                                                                 uint16_t endpointId);
    Windows::Foundation::IAsyncOperation<Windows::Foundation::Collections::IVectorView<Controller::MatterEndpointDescriptor>>
    DiscoverCamerasAsync(uint64_t nodeId);
    Windows::Foundation::IAsyncOperation<Controller::MatterCameraDiscoveryResult> TryDiscoverCamerasAsync(uint64_t nodeId);

private:
    std::shared_ptr<ControllerRuntime> mRuntime;
};

struct MatterCameraDiscoveryResult : MatterCameraDiscoveryResultT<MatterCameraDiscoveryResult>
{
    explicit MatterCameraDiscoveryResult(
        Windows::Foundation::Collections::IVectorView<Controller::MatterEndpointDescriptor> cameras) : mCameras(std::move(cameras)) {}
    explicit MatterCameraDiscoveryResult(Controller::MatterCameraFailure failure) : mFailure(std::move(failure)) {}
    bool Succeeded() const { return mCameras != nullptr; }
    Windows::Foundation::Collections::IVectorView<Controller::MatterEndpointDescriptor> Cameras() const { return mCameras; }
    Controller::MatterCameraFailure Failure() const { return mFailure; }
private:
    Windows::Foundation::Collections::IVectorView<Controller::MatterEndpointDescriptor> mCameras{ nullptr };
    Controller::MatterCameraFailure mFailure{ nullptr };
};

struct MatterControllerRecovery : MatterControllerRecoveryT<MatterControllerRecovery>
{
    explicit MatterControllerRecovery(Controller::MatterController controller);
    Windows::Foundation::IAsyncOperation<Controller::CommissionedNode> RecoverNodeAsync(uint64_t nodeId);

private:
    std::shared_ptr<ControllerRuntime> mRuntime;
};

struct MatterControllerNetworkCommissioning : MatterControllerNetworkCommissioningT<MatterControllerNetworkCommissioning>
{
    explicit MatterControllerNetworkCommissioning(Controller::MatterController controller);
    Windows::Foundation::IAsyncOperation<Controller::CommissionedNode>
    CommissionBleAsync(Controller::BleNetworkCommissioningParameters parameters);

private:
    std::shared_ptr<ControllerRuntime> mRuntime;
};

} // namespace winrt::Matter::Windows::Controller::implementation

namespace winrt::Matter::Windows::Controller::factory_implementation {

struct ControllerOptions : ControllerOptionsT<ControllerOptions, implementation::ControllerOptions>
{};
struct OnNetworkCommissioningParameters :
    OnNetworkCommissioningParametersT<OnNetworkCommissioningParameters, implementation::OnNetworkCommissioningParameters>
{};
struct BleCommissioningParameters :
    BleCommissioningParametersT<BleCommissioningParameters, implementation::BleCommissioningParameters>
{};
struct BleNetworkCommissioningParameters :
    BleNetworkCommissioningParametersT<BleNetworkCommissioningParameters, implementation::BleNetworkCommissioningParameters>
{};
struct WiFiNetworkCredentials :
    WiFiNetworkCredentialsT<WiFiNetworkCredentials, implementation::WiFiNetworkCredentials>
{};
struct ThreadNetworkCredentials :
    ThreadNetworkCredentialsT<ThreadNetworkCredentials, implementation::ThreadNetworkCredentials>
{};
struct AttributePath : AttributePathT<AttributePath, implementation::AttributePath>
{};
struct CommandPath : CommandPathT<CommandPath, implementation::CommandPath>
{};
struct EventPath : EventPathT<EventPath, implementation::EventPath>
{};
struct TimedInteractionOptions : TimedInteractionOptionsT<TimedInteractionOptions, implementation::TimedInteractionOptions>
{};
struct MatterController : MatterControllerT<MatterController, implementation::MatterController>
{};
struct MatterControllerRecovery : MatterControllerRecoveryT<MatterControllerRecovery, implementation::MatterControllerRecovery>
{};
struct MatterControllerCameraDiscovery :
    MatterControllerCameraDiscoveryT<MatterControllerCameraDiscovery, implementation::MatterControllerCameraDiscovery>
{};
struct MatterControllerCamera : MatterControllerCameraT<MatterControllerCamera, implementation::MatterControllerCamera> {};
struct MatterCameraVideoStreamOptions :
    MatterCameraVideoStreamOptionsT<MatterCameraVideoStreamOptions, implementation::MatterCameraVideoStreamOptions> {};
struct MatterCameraIceCandidate : MatterCameraIceCandidateT<MatterCameraIceCandidate, implementation::MatterCameraIceCandidate> {};
struct MatterCameraIceServer : MatterCameraIceServerT<MatterCameraIceServer, implementation::MatterCameraIceServer> {};
struct MatterControllerNetworkCommissioning :
    MatterControllerNetworkCommissioningT<MatterControllerNetworkCommissioning,
                                          implementation::MatterControllerNetworkCommissioning>
{};

} // namespace winrt::Matter::Windows::Controller::factory_implementation
