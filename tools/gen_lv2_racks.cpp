/*
 * The rack list, as Turtle.
 *
 * LV2 describes a plugin in a file the host reads before any code runs, so an
 * enumerated port has to name its options there - and the names live in C,
 * next to the racks they belong to. Writing them twice would mean a menu that
 * drifts out of step with the presets and nothing to notice it.
 *
 *   bencsynth-gen-racks > fragment.ttl
 */
#include "bs_engine.h"
#include <cstdio>

int main(void)
{
    const int n = bs::rackPresetCount();
    std::printf("\t\ta lv2:InputPort , lv2:ControlPort ;\n");
    std::printf("\t\tlv2:index 11 ;\n");
    std::printf("\t\tlv2:symbol \"rack\" ;\n");
    std::printf("\t\tlv2:name \"Rack\" ;\n");
    std::printf("\t\tlv2:default 0 ; lv2:minimum 0 ; lv2:maximum %d ;\n", n - 1);
    /* integer says it steps, enumeration says the steps are the only legal
     * values - LMMS draws that as a chooser rather than a knob. */
    std::printf("\t\tlv2:portProperty lv2:integer , lv2:enumeration ;\n");
    for (int i = 0; i < n; i++) {
        const bs::RackPreset *rp = bs::rackPresetAt(i);
        /* The predicate is repeated on every line and they are separated by
         * semicolons. A comma continues the SAME predicate with another
         * object, so "lv2:scalePoint [...] , lv2:scalePoint [...]" is a
         * predicate where an object belongs - which is a parse error, and the
         * validator then reports the cascade rather than the cause. */
        /* Typed explicitly. lv2:scalePoint's range is lv2:ScalePoint, and a
         * bare blank node with a label and a value satisfies a parser but not
         * a validator checking ranges. */
        std::printf("\t\tlv2:scalePoint [ a lv2:ScalePoint ; "
                    "rdfs:label \"%s\" ; rdf:value %d ] ;\n",
                    rp ? rp->name : "?", i);
    }
    return 0;
}
