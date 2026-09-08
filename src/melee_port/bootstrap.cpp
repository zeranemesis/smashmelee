#ifdef MELEE_BOOTSTRAP

#include "hsd/host_runtime.hpp"

#include "../port/ui/document.hpp"
#include "../port/ui/ui.hpp"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/lib/logging.hpp>
#include <port/main.h>

#include <memory>

namespace {

aurora::Module MeleeBootstrapLog("meleeboard::bootstrap");

const Rml::String kBootstrapDocument = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/melee_bootstrap.rcss" />
</head>
<body>
    <div class="backdrop" />
    <main>
        <div class="eyebrow">NATIVE PC PORT</div>
        <h1>MELEE BOARD</h1>
        <div class="rule" />
        <h2>HSD host graphics ready</h2>
        <p>The GALE01 v1.02 disc is mounted; HSD allocation, classes, scheduling, and GX state are verified.</p>
        <p class="next">Next milestone: import HSD scene initialization and reach Melee's first scene.</p>
    </main>
</body>
</rml>
)RML";

class MeleeBootstrapDocument final : public partyboard::ui::Document {
public:
    MeleeBootstrapDocument()
        : Document(kBootstrapDocument)
    {
    }
};

} // namespace

extern "C" int game_main(void)
{
    if (!meleeboard::hsd::initialize_host_runtime()) {
        MeleeBootstrapLog.error("HSD host runtime self-test failed");
        return 1;
    }
    MeleeBootstrapLog.info(
        "GALE01 mounted; HSD allocator and class model verified; entering bootstrap loop");
    partyboard::ui::push_document(std::make_unique<MeleeBootstrapDocument>());

    while (PartyBoard_IsRunning) {
        const AuroraEvent* event = aurora_update();
        while (event != nullptr && event->type != AURORA_NONE) {
            if (event->type == AURORA_EXIT) {
                PartyBoard_IsRunning = false;
                break;
            }
            if (event->type == AURORA_SDL_EVENT) {
                partyboard::ui::handle_event(event->sdl);
            }
            ++event;
        }

        if (!PartyBoard_IsRunning) {
            break;
        }
        if (!aurora_begin_frame()) {
            continue;
        }
        meleeboard::hsd::tick_host_runtime();
        partyboard::ui::update();
        aurora_end_frame();
    }

    meleeboard::hsd::shutdown_host_runtime();
    return 0;
}

#endif
