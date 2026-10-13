#pragma once

#include <winrt/Windows.Foundation.h>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

namespace winrt::Matter::Windows::Controller::implementation {

class CameraAudioAllocationOwnership
{
public:
    void Reserve(uint64_t node, uint16_t endpoint)
    {
        std::scoped_lock lock(mMutex);
        if (mEntries.contains({ node, endpoint }))
        {
            throw hresult_illegal_method_call(L"An audio allocation is already owned, pending, or uncertain on this endpoint.");
        }
        if (mEntries.size() >= 8)
        {
            throw hresult_error(winrt::hresult{ static_cast<int32_t>(0x8007000E) }, L"The audio ownership table is full.");
        }
        mEntries.emplace(Key{ node, endpoint }, Entry{});
    }

    void Commit(uint64_t node, uint16_t endpoint, uint16_t stream)
    {
        std::scoped_lock lock(mMutex);
        auto found = mEntries.find({ node, endpoint });
        if (found == mEntries.end() || found->second.state != State::Pending || stream == UINT16_MAX)
        {
            throw hresult_out_of_bounds(L"Audio allocation returned invalid ownership metadata.");
        }
        auto & entry = found->second;
        entry.stream = stream;
        entry.state = State::Owned;
    }

    void AllocationFailed(uint64_t node, uint16_t endpoint, bool knownRejection)
    {
        std::scoped_lock lock(mMutex);
        auto entry = mEntries.find({ node, endpoint });
        if (entry != mEntries.end() && entry->second.state == State::Pending)
        {
            if (knownRejection)
            {
                mEntries.erase(entry);
            }
            else
            {
                entry->second.state = State::Uncertain;
            }
        }
    }

    void StartRelease(uint64_t node, uint16_t endpoint, uint16_t stream)
    {
        std::scoped_lock lock(mMutex);
        auto entry = mEntries.find({ node, endpoint });
        if (entry == mEntries.end() || entry->second.state != State::Owned || entry->second.stream != stream)
        {
            throw hresult_illegal_method_call(L"Only this controller's owned audio stream may be deallocated, once.");
        }
        entry->second.state = State::Releasing;
    }

    void ReleaseFinished(uint64_t node, uint16_t endpoint, bool succeeded)
    {
        std::scoped_lock lock(mMutex);
        auto entry = mEntries.find({ node, endpoint });
        if (entry == mEntries.end() || entry->second.state != State::Releasing)
        {
            throw hresult_illegal_method_call(L"No audio deallocation is in progress.");
        }
        if (succeeded)
        {
            mEntries.erase(entry);
        }
        else
        {
            entry->second.state = State::Uncertain;
        }
    }

private:
    using Key = std::pair<uint64_t, uint16_t>;
    enum class State { Pending, Owned, Releasing, Uncertain };
    struct Entry
    {
        State state = State::Pending;
        std::optional<uint16_t> stream;
    };
    std::mutex mMutex;
    std::map<Key, Entry> mEntries;
};
}
