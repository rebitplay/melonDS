// Exercise the actual internal scheduler without a ROM or a test-only public API.
#include "melonds_dual.cpp"
#include <cstdio>
#include <cstdlib>
#include <future>

using namespace rebit;
using namespace std::chrono_literals;

void Check(bool condition, const char* message)
{
    if (condition) return;
    std::fprintf(stderr, "%s\n", message);
    CancelMultiplayerFrame();
    std::exit(1);
}

void CheckPlayers(int players)
{
    for (int player = 0; player < players; ++player)
    {
        auto slot = std::make_unique<Slot>();
        slot->context.id = player;
        State.slots.push_back(std::move(slot));
    }

    for (int owner = 0; owner < players; ++owner)
    {
        BeginMultiplayerFrame();
        State.multiplayerTurn.store(owner);
        auto* context = &State.slots[owner]->context;
        Check(EnterMultiplayerTurn(context, MultiplayerOperation::SendPacket), "Could not acquire turn");

        // The other console finishes while this console still owns a radio
        // operation. It must not change the done mask until the handoff, or
        // LeaveMultiplayerTurn can select an already-completed console.
        std::vector<std::future<void>> completions;
        // Reverse launch order differs from the fixed cyclic virtual order.
        for (int player = players - 1; player >= 0; --player)
            if (player != owner)
                completions.push_back(std::async(std::launch::async, [player] { CompleteMultiplayerFrame(player); }));
        const bool waited = completions.front().wait_for(100ms) == std::future_status::timeout;
        const bool maskUnchanged = State.multiplayerDoneMask.load() == 0;
        LeaveMultiplayerTurn(context, true);
        for (auto& completion : completions)
        {
            Check(completion.wait_for(1s) == std::future_status::ready, "Completion did not accept the handoff");
            completion.get();
        }
        Check(waited && maskUnchanged, "Console completed outside its scheduled turn");
        Check(State.multiplayerTurn.load() == owner, "Turn was left on a completed console");
        CompleteMultiplayerFrame(owner);
        Check(RuntimeAtCheckpointBoundary(), "Frame did not reach the snapshot boundary");
    }

    // A host waiting for a reply must yield to a guest that has no reply and
    // is finishing its frame, then resume without a watchdog cancellation.
    BeginMultiplayerFrame();
    NoteMultiplayerCommand(&State.slots[0]->context);
    std::vector<std::future<void>> completions;
    for (int player = players - 1; player > 0; --player)
        completions.push_back(std::async(std::launch::async, [player] { CompleteMultiplayerFrame(player); }));
    Check(EnterMultiplayerTurn(&State.slots[0]->context, MultiplayerOperation::RecvReplies),
        "Reply wait failed to accept guest frame completion");
    for (auto& completion : completions) completion.get();
    LeaveMultiplayerTurn(&State.slots[0]->context, true);
    CompleteMultiplayerFrame(0);
    Check(RuntimeAtCheckpointBoundary(), "Reply wait stranded a frame");
    State.slots.clear();
}

int main()
{
    for (int players = 2; players <= 4; ++players) CheckPlayers(players);
    std::puts("Rollback scheduler completion checks passed for 2, 3 and 4 consoles");
}
