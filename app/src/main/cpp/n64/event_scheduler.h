#pragma once

#include <cstdint>
#include <functional>
#include <queue>
#include <unordered_set>
#include <utility>
#include <vector>

namespace m64plus::n64 {

// A device-agnostic deterministic event queue. Time units are deliberately
// abstract: callers must supply researched hardware time conversions.
class EventScheduler final {
public:
    using Tick = std::uint64_t;
    using EventId = std::uint64_t;
    using Callback = std::function<void()>;

    Tick now() const noexcept { return now_; }

    EventId scheduleAt(Tick deadline, Callback callback) {
        if (!callback || deadline < now_) return 0U;
        const EventId id = nextId_++;
        events_.push(Event{deadline, nextSequence_++, id, std::move(callback)});
        pending_.insert(id);
        return id;
    }

    bool cancel(EventId id) {
        if (id == 0U || pending_.find(id) == pending_.end()) return false;
        return cancelled_.insert(id).second;
    }

    // Advances monotonically and runs all events due at or before target.
    // Events scheduled by callbacks for this same interval are also drained.
    bool advanceTo(Tick target) {
        if (target < now_) return false;
        while (!events_.empty() && events_.top().deadline <= target) {
            Event event = events_.top();
            events_.pop();
            pending_.erase(event.id);
            if (cancelled_.erase(event.id) != 0U) continue;
            now_ = event.deadline;
            event.callback();
        }
        now_ = target;
        return true;
    }

    bool hasPendingEvents() const noexcept { return !events_.empty(); }

    void reset() {
        events_ = {};
        cancelled_.clear();
        pending_.clear();
        now_ = 0U;
        nextId_ = 1U;
        nextSequence_ = 0U;
    }

private:
    struct Event {
        Tick deadline;
        std::uint64_t sequence;
        EventId id;
        Callback callback;
    };

    struct Later {
        bool operator()(const Event& left, const Event& right) const noexcept {
            if (left.deadline != right.deadline)
                return left.deadline > right.deadline;
            return left.sequence > right.sequence;
        }
    };

    Tick now_ = 0U;
    EventId nextId_ = 1U;
    std::uint64_t nextSequence_ = 0U;
    std::priority_queue<Event, std::vector<Event>, Later> events_;
    std::unordered_set<EventId> cancelled_;
    std::unordered_set<EventId> pending_;
};

} // namespace m64plus::n64
