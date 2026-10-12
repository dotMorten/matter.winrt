#pragma once

#include <winrt/Windows.Foundation.h>
#include <lib/support/logging/CHIPLogging.h>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace winrt::Matter::Windows::Controller::implementation {

// Own the reference until the asynchronous result is delivered, including cancellation at completion.
class PendingCameraVideoStream : public std::enable_shared_from_this<PendingCameraVideoStream>
{
public:
    explicit PendingCameraVideoStream(std::function<void(uint16_t)> release) : mRelease(std::move(release)) {}

    void Cancel()
    {
        bool release = false;
        {
            std::scoped_lock lock(mMutex);
            mCanceled = true;
            release = mStream.has_value();
        }
        if (release)
        {
            ReleaseCanceled(shared_from_this());
        }
    }

    void Record(uint16_t stream)
    {
        bool canceled = false;
        {
            std::scoped_lock lock(mMutex);
            mStream = stream;
            canceled = mCanceled;
        }
        if (canceled)
        {
            ReleaseCanceled(shared_from_this());
        }
    }

    void Release()
    {
        std::unique_lock lock(mMutex);
        if (mReleaseStarted)
        {
            mFinished.wait(lock, [&] { return mReleaseFinished; });
            if (mFailure)
            {
                std::rethrow_exception(mFailure);
            }
            return;
        }
        if (!mStream)
        {
            return;
        }
        mReleaseStarted = true;
        const uint16_t stream = *mStream;
        lock.unlock();
        std::exception_ptr failure;
        try
        {
            mRelease(stream);
        }
        catch (...)
        {
            failure = std::current_exception();
        }
        lock.lock();
        mFailure = failure;
        mReleaseFinished = true;
        if (!failure)
        {
            mStream.reset();
        }
        lock.unlock();
        mFinished.notify_all();
        if (failure)
        {
            std::rethrow_exception(failure);
        }
    }

private:
    static winrt::fire_and_forget ReleaseCanceled(std::shared_ptr<PendingCameraVideoStream> owner)
    {
        co_await winrt::resume_background();
        try
        {
            owner->Release();
        }
        catch (winrt::hresult_error const & error)
        {
            ChipLogError(Controller, "Canceled video allocation cleanup failed (HRESULT 0x%08lx); remote ownership is uncertain.",
                         static_cast<unsigned long>(error.code()));
        }
        catch (std::exception const &)
        {
            ChipLogError(Controller, "Canceled video allocation cleanup failed; remote ownership is uncertain.");
        }
    }

    std::function<void(uint16_t)> mRelease;
    std::mutex mMutex;
    std::condition_variable mFinished;
    std::optional<uint16_t> mStream;
    std::exception_ptr mFailure;
    bool mCanceled = false;
    bool mReleaseStarted = false;
    bool mReleaseFinished = false;
};
}
