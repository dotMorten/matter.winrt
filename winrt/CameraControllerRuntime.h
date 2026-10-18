#pragma once

#include <access/AccessControl.h>
#include <app/persistence/DefaultAttributePersistenceProvider.h>
#include <controller/CHIPDeviceController.h>
#include <controller/webrtc/CameraProviderClient.h>
#include <controller/webrtc/CameraRequestorEndpoint.h>
#include <crypto/CHIPCryptoPAL.h>
#include <data-model-providers/codedriven/CodeDrivenDataModelProvider.h>
#include <platform/CHIPDeviceLayer.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace winrt::Matter::Windows::Controller::implementation {

class CameraStackLock
{
public:
    CameraStackLock() { chip::DeviceLayer::PlatformMgr().LockChipStack(); }
    ~CameraStackLock() { chip::DeviceLayer::PlatformMgr().UnlockChipStack(); }
    CameraStackLock(const CameraStackLock &) = delete;
    CameraStackLock & operator=(const CameraStackLock &) = delete;
};

/// Access policy belongs only to this opt-in controller host. Existing global
/// access-control instances are never replaced.
class CameraControllerRuntime : private chip::Access::AccessControl::Delegate,
                                private chip::Access::AccessControl::DeviceTypeResolver
{
public:
    using Cluster = chip::Controller::CameraSignalingCluster;
    using Client = chip::Controller::CameraProviderClient;

    struct IceServerConfiguration
    {
        std::vector<std::string> urls;
        std::optional<std::string> username;
        std::optional<std::string> credential;
        std::optional<uint16_t> certificateAuthorityId;
        IceServerConfiguration() = default;
        IceServerConfiguration(IceServerConfiguration &&) = default;
        IceServerConfiguration & operator=(IceServerConfiguration &&) = default;
        IceServerConfiguration(IceServerConfiguration const &) = delete;
        ~IceServerConfiguration()
        {
            for (auto * field : { &username, &credential })
            {
                if (field->has_value() && !field->value().empty())
                {
                    chip::Crypto::ClearSecretData(reinterpret_cast<uint8_t *>(field->value().data()), field->value().size());
                }
            }
        }
    };

    explicit CameraControllerRuntime(chip::PersistentStorageDelegate & storage) :
        mStorage(storage), mProvider(storage, mPersistence)
    {}

    CHIP_ERROR Init(chip::PersistentStorageDelegate & storage)
    {
        size_t count = 0;
        VerifyOrReturnError(chip::Access::GetAccessControl().GetEntryCount(count) == CHIP_ERROR_INCORRECT_STATE,
                            CHIP_ERROR_INCORRECT_STATE);
        ReturnErrorOnFailure(mPersistence.Init(&storage));
        ReturnErrorOnFailure(mAccess.Init(this, *this));
        chip::Access::SetAccessControl(mAccess);
        mAccessInstalled = true;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR Shutdown()
    {
        // Detach before controller shutdown can forget its fabric and delete stored bindings.
        mBindings.Shutdown();
        mBindingsInitialized = false;
        mClient.reset();
        if (mEndpoint)
        {
            ReturnErrorOnFailure(mEndpoint->Unregister());
            mEndpoint.reset();
        }
        mActiveHandle = 0;
        if (mAccessInstalled)
        {
            chip::Access::ResetAccessControlToDefault();
            mAccess.Finish();
            mAccessInstalled = false;
        }
        return CHIP_NO_ERROR;
    }

    chip::app::DataModel::Provider & Provider() { return mProvider; }
    uint64_t ActiveHandle() const { return mActiveHandle; }
    uint16_t RequestorEndpoint() const { return mEndpoint ? mEndpoint->GetEndpointId() : chip::kInvalidEndpointId; }

    CHIP_ERROR Create(chip::FabricTable & fabrics, chip::CASESessionManager & sessions, chip::System::Layer & system,
                      chip::ScopedNodeId peer,
                      uint16_t endpoint, uint16_t videoStream, uint64_t & handle)
    {
        return Create(fabrics, sessions, system, peer, endpoint, Cluster::StreamSelection::ExplicitId(videoStream),
                      Cluster::StreamSelection::Absent(), handle);
    }

    CHIP_ERROR Create(chip::FabricTable & fabrics, chip::CASESessionManager & sessions, chip::System::Layer & system,
                      chip::ScopedNodeId peer, uint16_t endpoint, Cluster::StreamSelection video,
                      Cluster::StreamSelection audio, uint64_t & handle)
    {
        VerifyOrReturnError(mActiveHandle == 0, CHIP_ERROR_BUSY);
        VerifyOrReturnError(mNextEndpoint != chip::kInvalidEndpointId && mNextHandle != 0,
                            CHIP_IM_GLOBAL_STATUS(ResourceExhausted));
        if (!mBindingsInitialized)
        {
            chip::app::Clusters::Binding::ManagerInitParams params;
            params.mFabricTable = &fabrics;
            params.mCASESessionManager = &sessions;
            params.mStorage = &mStorage;
            params.mEstablishConnectionOnInit = false;
            ReturnErrorOnFailure(mBindings.Init(params));
            mBindingsInitialized = true;
        }
        mEndpoint = std::make_unique<chip::Controller::CameraRequestorEndpoint>(
            mNextEndpoint++, chip::Span<const chip::app::DataModel::DeviceTypeEntry>(mDeviceTypes), fabrics, system,
            mBindings, chip::DeviceLayer::PlatformMgr());
        CHIP_ERROR error = mEndpoint->Register(mProvider, mAccess);
        if (error != CHIP_NO_ERROR)
        {
            mEndpoint.reset();
            return error;
        }
        error = mEndpoint->Signaling().CreateSession(peer, endpoint, video, audio, mNativeHandle);
        if (error != CHIP_NO_ERROR)
        {
            LogErrorOnFailure(mEndpoint->Unregister());
            mEndpoint.reset();
            return error;
        }
        mClient = std::make_unique<Client>(mEndpoint->Signaling(), mNativeHandle);
        mPeerNode = peer.GetNodeId();
        mActiveHandle = mNextHandle++;
        handle = mActiveHandle;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR Validate(uint64_t handle) const
    {
        VerifyOrReturnError(handle != 0 && handle == mActiveHandle && mEndpoint && mClient, CHIP_ERROR_INCORRECT_STATE);
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetPeerNode(uint64_t handle, chip::NodeId & node) const
    {
        ReturnErrorOnFailure(Validate(handle));
        node = mPeerNode;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetSessionState(uint64_t handle, Cluster::SessionState & state) const
    {
        ReturnErrorOnFailure(Validate(handle));
        return mEndpoint->Signaling().GetSessionState(mNativeHandle, state);
    }

    CHIP_ERROR GetSessionInfo(uint64_t handle, Cluster::SessionInfo & info) const
    {
        ReturnErrorOnFailure(Validate(handle));
        return mEndpoint->Signaling().GetSessionInfo(mNativeHandle, info);
    }

    std::optional<Cluster::Signal> TakeSignal(uint64_t handle)
    {
        auto signal = mEndpoint->Signaling().TakeSignal();
        if (signal)
        {
            signal->handle = handle;
        }
        return signal;
    }

    CHIP_ERROR CloseLocal(uint64_t handle)
    {
        if (handle != mActiveHandle)
        {
            return CHIP_NO_ERROR;
        }
        mClient.reset();
        ReturnErrorOnFailure(mEndpoint->Signaling().CloseSession(mNativeHandle));
        ReturnErrorOnFailure(mEndpoint->Unregister());
        mEndpoint.reset();
        mActiveHandle = 0;
        return CHIP_NO_ERROR;
    }

    class CommandOperation
    {
    public:
        CommandOperation(Client & client, Cluster & cluster, uint64_t nativeHandle, Client::Command command,
                         std::string sdp, std::vector<Cluster::Candidate> candidates,
                         std::vector<IceServerConfiguration> iceServers) :
            mClient(client), mCluster(cluster), mNativeHandle(nativeHandle), mCommand(command),
            mSdp(std::move(sdp)), mCandidates(std::move(candidates)), mIceServers(std::move(iceServers)),
            mConnected(Connected, this), mFailed(Failed, this)
        {}
        ~CommandOperation()
        {
            CameraStackLock lock;
            mConnected.Cancel();
            mFailed.Cancel();
        }
        CHIP_ERROR Run(chip::Controller::DeviceCommissioner & commissioner, chip::NodeId node,
                       std::shared_ptr<std::atomic_bool> const & canceled)
        {
            {
                CameraStackLock lock;
                ReturnErrorOnFailure(commissioner.GetConnectedDevice(node, &mConnected, &mFailed,
                                                                      chip::TransportPayloadCapability::kLargePayload));
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (true)
            {
                {
                    CameraStackLock lock;
                    if (canceled && canceled->load(std::memory_order_acquire))
                    {
                        mConnected.Cancel();
                        mFailed.Cancel();
                        mClient.Cancel();
                        (void) mClient.TakeResult();
                        return CHIP_ERROR_CANCELLED;
                    }
                    const auto connectionError = mConnectionError;
                    if (connectionError != CHIP_NO_ERROR)
                    {
                        return connectionError;
                    }
                    if (auto result = mClient.TakeResult())
                    {
                        return result->error;
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        mConnected.Cancel();
                        mFailed.Cancel();
                        mClient.Cancel();
                        (void) mClient.TakeResult();
                        return CHIP_ERROR_TIMEOUT;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    private:
        static void Connected(void * context, chip::Messaging::ExchangeManager & exchanges, const chip::SessionHandle & session)
        {
            auto & self = *static_cast<CommandOperation *>(context);
            Cluster::SessionInfo info;
            self.mConnectionError = self.mCommand == Client::Command::End
                ? self.mCluster.GetSessionInfoForEnd(self.mNativeHandle, info)
                : self.mCluster.GetSessionInfo(self.mNativeHandle, info);
            if (self.mConnectionError != CHIP_NO_ERROR)
            {
                return;
            }
            if (self.mCommand == Client::Command::Offer)
            {
                std::array<Client::IceServer, Client::kMaxIceServers> servers;
                std::array<std::array<chip::CharSpan, Client::kMaxIceUrls>, Client::kMaxIceServers> urls;
                if (self.mIceServers.size() > servers.size())
                {
                    self.mConnectionError = CHIP_ERROR_INVALID_ARGUMENT;
                    return;
                }
                for (size_t i = 0; i < self.mIceServers.size(); ++i)
                {
                    auto const & source = self.mIceServers[i];
                    if (source.urls.size() > urls[i].size())
                    {
                        self.mConnectionError = CHIP_ERROR_INVALID_ARGUMENT;
                        return;
                    }
                    for (size_t j = 0; j < source.urls.size(); ++j)
                    {
                        urls[i][j] = chip::CharSpan(source.urls[j].data(), source.urls[j].size());
                    }
                    servers[i].URLs = chip::app::DataModel::List<const chip::CharSpan>(urls[i].data(), source.urls.size());
                    if (source.username)
                    {
                        servers[i].username.SetValue(chip::CharSpan(source.username->data(), source.username->size()));
                    }
                    if (source.credential)
                    {
                        servers[i].credential.SetValue(chip::CharSpan(source.credential->data(), source.credential->size()));
                    }
                    if (source.certificateAuthorityId)
                    {
                        servers[i].caid.SetValue(*source.certificateAuthorityId);
                    }
                }
                self.mConnectionError = self.mClient.SendOffer(exchanges, session,
                    chip::CharSpan(self.mSdp.data(), self.mSdp.size()),
                    chip::Span<const Client::IceServer>(servers.data(), self.mIceServers.size()));
            }
            else if (self.mCommand == Client::Command::Candidates)
            {
                self.mConnectionError = self.mClient.SendCandidates(exchanges, session,
                    chip::Span<const Cluster::Candidate>(self.mCandidates.data(), self.mCandidates.size()));
            }
            else
            {
                self.mConnectionError = self.mClient.SendEnd(exchanges, session,
                    chip::app::Clusters::Globals::WebRTCEndReasonEnum::kUserHangup);
            }
        }
        static void Failed(void * context, const chip::ScopedNodeId &, CHIP_ERROR error)
        {
            static_cast<CommandOperation *>(context)->mConnectionError = error;
        }
        Client & mClient;
        Cluster & mCluster;
        uint64_t mNativeHandle;
        Client::Command mCommand;
        std::string mSdp;
        std::vector<Cluster::Candidate> mCandidates;
        std::vector<IceServerConfiguration> mIceServers;
        CHIP_ERROR mConnectionError = CHIP_NO_ERROR;
        chip::Callback::Callback<chip::OnDeviceConnected> mConnected;
        chip::Callback::Callback<chip::OnDeviceConnectionFailure> mFailed;
    };

    CHIP_ERROR Send(chip::Controller::DeviceCommissioner & commissioner, uint64_t handle, Client::Command command,
                    std::string sdp, std::vector<Cluster::Candidate> candidates,
                    std::shared_ptr<std::atomic_bool> const & canceled,
                    std::vector<IceServerConfiguration> iceServers = {})
    {
        Cluster::SessionInfo info;
        {
            CameraStackLock lock;
            ReturnErrorOnFailure(Validate(handle));
            const auto error = command == Client::Command::End
                ? mEndpoint->Signaling().GetSessionInfoForEnd(mNativeHandle, info)
                : mEndpoint->Signaling().GetSessionInfo(mNativeHandle, info);
            if (command == Client::Command::End && error == CHIP_ERROR_NOT_FOUND)
            {
                // A remote End already retired this session.
                return CHIP_NO_ERROR;
            }
            ReturnErrorOnFailure(error);
            if (command == Client::Command::End && !info.protocolId.has_value())
            {
                // No authenticated protocol identity is available to end.
                return CHIP_NO_ERROR;
            }
        }
        CommandOperation operation(*mClient, mEndpoint->Signaling(), mNativeHandle, command, std::move(sdp),
            std::move(candidates), std::move(iceServers));
        return operation.Run(commissioner, info.metadata.peerNodeID, canceled);
    }

private:
    CHIP_ERROR Check(const chip::Access::SubjectDescriptor & subject, const chip::Access::RequestPath & path,
                     chip::Access::Privilege privilege) override
    {
        return mEndpoint ? mEndpoint->CheckAccess(subject, path, privilege) : CHIP_ERROR_ACCESS_DENIED;
    }
    bool IsDeviceTypeOnEndpoint(chip::DeviceTypeId type, chip::EndpointId endpoint) override
    {
        return mEndpoint && endpoint == mEndpoint->GetEndpointId() && type == mDeviceTypes[0].deviceTypeId;
    }

    chip::PersistentStorageDelegate & mStorage;
    chip::app::DefaultAttributePersistenceProvider mPersistence;
    chip::app::CodeDrivenDataModelProvider mProvider;
    chip::app::Clusters::Binding::Manager mBindings;
    bool mBindingsInitialized = false;
    chip::Access::AccessControl mAccess;
    bool mAccessInstalled = false;
    // Checked-in endpoint composition, not a claim of complete product certification.
    const chip::app::DataModel::DeviceTypeEntry mDeviceTypes[1]{ { 0x0147, 1 } };
    std::unique_ptr<chip::Controller::CameraRequestorEndpoint> mEndpoint;
    std::unique_ptr<Client> mClient;
    uint16_t mNextEndpoint = 1;
    uint64_t mNextHandle = 1;
    uint64_t mActiveHandle = 0;
    uint64_t mNativeHandle = 0;
    chip::NodeId mPeerNode = chip::kUndefinedNodeId;
};

} // namespace winrt::Matter::Windows::Controller::implementation
