#include "progress-bar.hh"
#include "util.hh"
#include "sync.hh"
#include "store-api.hh"

#include <map>
#include <atomic>

#include <sys/ioctl.h>

namespace nix {

static std::string getS(const std::vector<Logger::Field> & fields, size_t n)
{
    assert(n < fields.size());
    assert(fields[n].type == Logger::Field::tString);
    return fields[n].s;
}

static uint64_t getI(const std::vector<Logger::Field> & fields, size_t n)
{
    assert(n < fields.size());
    assert(fields[n].type == Logger::Field::tInt);
    return fields[n].i;
}

class ProgressBar : public Logger
{
private:

    struct ActInfo
    {
        std::string s, s2;
        ActivityType type = actUnknown;
        uint64_t done = 0;
        uint64_t expected = 0;
        uint64_t running = 0;
        uint64_t failed = 0;
        std::map<ActivityType, uint64_t> expectedByType;
    };

    struct ActivitiesByType
    {
        std::map<ActivityId, std::list<ActInfo>::iterator> its;
        uint64_t done = 0;
        uint64_t expected = 0;
        uint64_t failed = 0;
    };

    struct State
    {
        std::list<ActInfo> activities;
        std::map<ActivityId, std::list<ActInfo>::iterator> its;

        std::map<ActivityType, ActivitiesByType> activitiesByType;

        uint64_t filesLinked = 0, bytesLinked = 0;

        uint64_t corruptedPaths = 0, untrustedPaths = 0;
    };

    Sync<State> state_;

    int width = 0;

public:

    ProgressBar()
    {
        struct winsize ws;
        if (ioctl(1, TIOCGWINSZ, &ws) == 0)
            width = ws.ws_col;
    }

    ~ProgressBar()
    {
        auto state(state_.lock());
        std::string status = getStatus(*state);
        writeToStderr("\r\e[K");
        if (status != "")
            writeToStderr("[" + status + "]\n");
    }

    void log(Verbosity lvl, const FormatOrString & fs) override
    {
        auto state(state_.lock());
        log(*state, lvl, fs.s);
    }

    void log(State & state, Verbosity lvl, const std::string & s)
    {
        writeToStderr("\r\e[K" + s + "\n");
        update(state);
    }

    void startActivity(ActivityId act, ActivityType type, const std::string & s) override
    {
        auto state(state_.lock());

        state->activities.emplace_back(ActInfo{s, "", type});
        auto i = std::prev(state->activities.end());
        state->its.emplace(act, i);
        state->activitiesByType[type].its.emplace(act, i);

        update(*state);
    }

    void stopActivity(ActivityId act) override
    {
        auto state(state_.lock());

        auto i = state->its.find(act);
        if (i != state->its.end()) {
            auto & actByType = state->activitiesByType[i->second->type];
            actByType.done += i->second->done;
            actByType.failed += i->second->failed;

            for (auto & j : i->second->expectedByType)
                state->activitiesByType[j.first].expected -= j.second;

            actByType.its.erase(act);
            state->activities.erase(i->second);
            state->its.erase(i);
        }

        update(*state);
    }

    void progress(ActivityId act, uint64_t done = 0, uint64_t expected = 0, uint64_t running = 0, uint64_t failed = 0) override
    {
        auto state(state_.lock());

        auto i = state->its.find(act);
        assert(i != state->its.end());
        ActInfo & actInfo = *i->second;
        actInfo.done = done;
        actInfo.expected = expected;
        actInfo.running = running;
        actInfo.failed = failed;

        update(*state);
    }

    void setExpected(ActivityId act, ActivityType type, uint64_t expected) override
    {
        auto state(state_.lock());

        auto i = state->its.find(act);
        assert(i != state->its.end());
        ActInfo & actInfo = *i->second;
        auto & j = actInfo.expectedByType[type];
        state->activitiesByType[type].expected -= j;
        j = expected;
        state->activitiesByType[type].expected += j;

        update(*state);
    }

    void result(ActivityId act, ResultType type, const std::vector<Field> & fields) override
    {
        auto state(state_.lock());

        if (type == resFileLinked) {
            state->filesLinked++;
            state->bytesLinked += getI(fields, 0);
            update(*state);
        }

        else if (type == resBuildLogLine) {
            auto s2 = trim(getS(fields, 0));
            if (!s2.empty()) {
                auto i = state->its.find(act);
                assert(i != state->its.end());
                ActInfo info = *i->second;
                state->activities.erase(i->second);
                info.s2 = s2;
                state->activities.emplace_back(info);
                i->second = std::prev(state->activities.end());
                update(*state);
            }
        }

        else if (type == resUntrustedPath) {
            state->untrustedPaths++;
            update(*state);
        }

        else if (type == resCorruptedPath) {
            state->corruptedPaths++;
            update(*state);
        }
    }

    void update()
    {
        auto state(state_.lock());
        update(*state);
    }

    void update(State & state)
    {
        std::string line = "\r";

        std::string status = getStatus(state);
        if (!status.empty()) {
            line += '[';
            line += status;
            line += "]";
        }

        if (!state.activities.empty()) {
            if (!status.empty()) line += " ";
            auto i = state.activities.rbegin();

            while (i != state.activities.rend() && i->s.empty() && i->s2.empty())
                ++i;

            if (i != state.activities.rend()) {
                line += i->s;
                if (!i->s2.empty()) {
                    if (!i->s.empty()) line += ": ";
                    line += i->s2;
                }
            }
        }

        line += "\e[K";
        writeToStderr(std::string(line, 0, width - 1));
    }

    std::string getStatus(State & state)
    {
        auto MiB = 1024.0 * 1024.0;

        std::string res;

        auto renderActivity = [&](ActivityType type, const std::string & itemFmt, const std::string & numberFmt = "%d", double unit = 1) {
            auto & act = state.activitiesByType[type];
            uint64_t done = act.done, expected = act.done, running = 0, failed = act.failed;
            for (auto & j : act.its) {
                done += j.second->done;
                expected += j.second->expected;
                running += j.second->running;
                failed += j.second->failed;
            }

            expected = std::max(expected, act.expected);

            std::string s;

            if (running || done || expected || failed) {
                if (running)
                    s = fmt(ANSI_BLUE + numberFmt + ANSI_NORMAL "/" ANSI_GREEN + numberFmt + ANSI_NORMAL "/" + numberFmt,
                        running / unit, done / unit, expected / unit);
                else if (expected != done)
                    s = fmt(ANSI_GREEN + numberFmt + ANSI_NORMAL "/" + numberFmt,
                        done / unit, expected / unit);
                else
                    s = fmt(done ? ANSI_GREEN + numberFmt + ANSI_NORMAL : numberFmt, done / unit);
                s = fmt(itemFmt, s);

                if (failed)
                    s += fmt(" (" ANSI_RED "%d failed" ANSI_NORMAL ")", failed / unit);
            }

            return s;
        };

        auto showActivity = [&](ActivityType type, const std::string & itemFmt, const std::string & numberFmt = "%d", double unit = 1) {
            auto s = renderActivity(type, itemFmt, numberFmt, unit);
            if (s.empty()) return;
            if (!res.empty()) res += ", ";
            res += s;
        };

        showActivity(actBuilds, "%s built");

        auto s1 = renderActivity(actCopyPaths, "%s copied");
        auto s2 = renderActivity(actCopyPath, "%s MiB", "%.1f", MiB);

        if (!s1.empty() || !s2.empty()) {
            if (!res.empty()) res += ", ";
            if (s1.empty()) res += "0 copied"; else res += s1;
            if (!s2.empty()) { res += " ("; res += s2; res += ')'; }
        }

        showActivity(actDownload, "%s MiB DL", "%.1f", MiB);

        {
            auto s = renderActivity(actOptimiseStore, "%s paths optimised");
            if (s != "") {
                s += fmt(", %.1f MiB / %d inodes freed", state.bytesLinked / MiB, state.filesLinked);
                if (!res.empty()) res += ", ";
                res += s;
            }
        }

        // FIXME: don't show "done" paths in green.
        showActivity(actVerifyPaths, "%s paths verified");

        if (state.corruptedPaths) {
            if (!res.empty()) res += ", ";
            res += fmt(ANSI_RED "%d corrupted" ANSI_NORMAL, state.corruptedPaths);
        }

        if (state.untrustedPaths) {
            if (!res.empty()) res += ", ";
            res += fmt(ANSI_RED "%d untrusted" ANSI_NORMAL, state.untrustedPaths);
        }

        return res;
    }
};

StartProgressBar::StartProgressBar()
{
    if (isatty(STDERR_FILENO)) {
        prev = logger;
        logger = new ProgressBar();
    }
}

StartProgressBar::~StartProgressBar()
{
    if (prev) {
        auto bar = logger;
        logger = prev;
        delete bar;
    }
}

}
