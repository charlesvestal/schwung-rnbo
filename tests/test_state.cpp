// Host-side tests for parameter keys and the `state` blob.
//
// The host is a dlopen loader with no dependency on the RNBO SDK, so it is
// compiled straight into this binary (rnbo_host.cpp is included) and its device
// API pointers are pointed at a fake device that keeps values in an array.
// Native build, no cross toolchain: see scripts/test.sh.

#include "../src/dsp/rnbo_host.cpp"

#include <cassert>
#include <iostream>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { g_failures++; \
    std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; } } while (0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if (!(_a == _b)) { g_failures++; \
    std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #a " == " #b "\n  got: " << _a << "\n  want: " << _b << "\n"; } } while (0)

/* A fake RNBO device: parameter values by index. */
struct FakeDevice {
    std::vector<double> values;
    int sets = 0;
};
static double fake_get_param(rnbo_device_t d, int index) {
    auto *f = (FakeDevice*)d;
    return (index >= 0 && index < (int)f->values.size()) ? f->values[index] : 0.0;
}
static void fake_set_param(rnbo_device_t d, int index, double v) {
    auto *f = (FakeDevice*)d;
    if (index >= 0 && index < (int)f->values.size()) { f->values[index] = v; f->sets++; }
}

/* description.json as RNBO exports it: a visible param, a hidden one in a
 * subpatcher, a signal param (not a value) and an enum. Indexes are the RNBO
 * parameter indexes, which skip nothing, so the signal param is index 2. */
static const char *DESC = R"JSON({
  "parameters": [
    {"paramId":"root","name":"root","displayName":"Root","minimum":12,"maximum":96,"initialValue":36,
     "exponent":1,"steps":0,"isEnum":false,"index":0,"visible":true,"type":"ParameterTypeNumber"},
    {"paramId":"routing/send1to2","name":"send1to2","minimum":0,"maximum":1,"initialValue":1,
     "exponent":1,"steps":0,"isEnum":false,"index":1,"visible":false,"type":"ParameterTypeNumber"},
    {"paramId":"/signals/freq","name":"freq","index":2,"visible":false,"type":"ParameterTypeSignal"},
    {"paramId":"wave","name":"wave","minimum":0,"maximum":2,"initialValue":0,"exponent":1,"steps":3,
     "isEnum":true,"enumValues":["sine","saw","square"],"index":3,"visible":true,"type":"ParameterTypeNumber"}
  ]
})JSON";

static RnboDevice make_device(const char *name) {
    RnboDevice dev{};
    dev.name = name;
    dev.api_get_param = fake_get_param;
    dev.api_set_param = fake_set_param;
    split_device_params(dev, parse_params_from_description(DESC));
    return dev;
}

static void add_node(RnboInstance &inst, int instance_index, int device_index, FakeDevice *fake) {
    GraphNode node{};
    node.instance_index = instance_index;
    node.patcher_name = "Noise Mixer";
    node.device_index = device_index;
    node.handle = (rnbo_device_t)fake;
    inst.graph.nodes.push_back(std::move(node));
}

static std::string get(RnboInstance &inst, const char *key, int buf_len = 4096) {
    std::string buf(buf_len, '\0');
    int n = rnbo_get_param(&inst, key, &buf[0], buf_len);
    if (n < 0) return "<-1>";
    buf.resize(n);
    return buf;
}

int main() {
    /* --- description parsing keeps hidden params and drops signal params --- */
    RnboDevice dev = make_device("noise-mixer");
    CHECK_EQ(dev.all_params.size(), (size_t)3);
    CHECK_EQ(dev.params.size(), (size_t)2);
    CHECK_EQ(dev.all_params[1].paramId, std::string("routing/send1to2"));
    CHECK_EQ(dev.all_params[1].index, 1);
    CHECK(!dev.all_params[1].visible);
    CHECK_EQ(dev.params[1].paramId, std::string("wave"));
    CHECK_EQ(dev.params[1].index, 3);

    /* --- an instance with two nodes of the same device --- */
    RnboInstance inst{};
    inst.graph_mode = true;
    inst.volume = 0.7f;
    inst.devices.push_back(dev);
    FakeDevice a{{36.0, 1.0, 0.0, 2.0}};
    FakeDevice b{{48.0, 0.25, 0.0, 1.0}};
    add_node(inst, 0, 0, &a);
    add_node(inst, 3, 0, &b);   /* instance indexes need not be dense */
    build_graph_ui(&inst);

    /* --- chain_params offers the visible params only, state carries all --- */
    std::string cp = get(inst, "chain_params");
    CHECK(cp.find("\"key\":\"0_root\"") != std::string::npos);
    CHECK(cp.find("\"key\":\"3_wave\"") != std::string::npos);
    CHECK(cp.find("send1to2") == std::string::npos);

    std::string state = get(inst, "state");
    CHECK_EQ(state, std::string(
        "{\"volume\":0.7,\"0_root\":36,\"0_routing-send1to2\":1,\"0_wave\":2,"
        "\"3_root\":48,\"3_routing-send1to2\":0.25,\"3_wave\":1}"));

    /* --- per-key access reaches hidden params too --- */
    CHECK_EQ(get(inst, "0_routing-send1to2"), std::string("1"));
    rnbo_set_param(&inst, "3_routing-send1to2", "0.5");
    CHECK_EQ(b.values[1], 0.5);
    CHECK_EQ(get(inst, "3_routing-send1to2"), std::string("0.5"));
    rnbo_set_param(&inst, "9_root", "1");          /* no such instance: ignored */
    rnbo_set_param(&inst, "0_nope", "1");          /* no such param: ignored */
    rnbo_set_param(&inst, "_root", "1");           /* no instance index: ignored */
    CHECK_EQ(get(inst, "9_root"), std::string(""));
    CHECK_EQ(a.values[0], 36.0);

    /* --- round trip: change everything, write the saved blob back --- */
    a.values = {60.0, 0.0, 0.0, 0.0};
    b.values = {61.0, 0.0, 0.0, 0.0};
    inst.volume = 0.1f;
    int sets_before = a.sets + b.sets;
    rnbo_set_param(&inst, "state", state.c_str());
    CHECK_EQ(a.values[0], 36.0);
    CHECK_EQ(a.values[1], 1.0);
    CHECK_EQ(a.values[3], 2.0);
    CHECK_EQ(b.values[0], 48.0);
    CHECK_EQ(b.values[1], 0.25);
    CHECK_EQ(b.values[3], 1.0);
    CHECK_EQ(a.values[2], 0.0);                     /* the signal param is never written */
    CHECK_EQ(a.sets + b.sets - sets_before, 6);
    CHECK(fabs(inst.volume - 0.7f) < 1e-6);
    CHECK_EQ(get(inst, "state"), state);            /* serialize, apply, serialize: identical */

    /* --- tolerant parsing: whitespace, quoted numbers, unknown keys, odd values --- */
    a.values = {0, 0, 0, 0};
    inst.volume = 0.0f;
    rnbo_set_param(&inst, "state",
        " {\n  \"0_root\" : \"40\" ,\n  \"volume\": 2.5,\n"
        "  \"0_gone\": 7, \"0_wave\": true, \"0_routing-send1to2\": {\"nested\": [1, \"}\"]},\n"
        "  \"3_wave\": \"abc\", \"3_root\": 1e1, \"0_routing-send1to2\": \"0.75x\"\n }");
    CHECK_EQ(a.values[0], 40.0);                    /* quoted number applied */
    CHECK(fabs(inst.volume - 1.0f) < 1e-6);         /* clamped like the volume key */
    CHECK_EQ(a.values[3], 0.0);                     /* true: skipped */
    CHECK_EQ(a.values[1], 0.0);                     /* nested object, then "0.75x": skipped */
    CHECK_EQ(b.values[0], 10.0);                    /* exponent form applied */
    CHECK_EQ(b.values[3], 1.0);                     /* "abc": skipped, previous value kept */

    /* --- garbage never writes --- */
    sets_before = a.sets + b.sets;
    rnbo_set_param(&inst, "state", "");
    rnbo_set_param(&inst, "state", "not json");
    rnbo_set_param(&inst, "state", "{");
    rnbo_set_param(&inst, "state", "{\"0_root\"");
    rnbo_set_param(&inst, "state", "{\"0_root\": }");
    rnbo_set_param(&inst, "state", "[1,2]");
    CHECK_EQ(a.sets + b.sets - sets_before, 0);

    /* --- a blob that does not fit is refused, not truncated --- */
    std::string now = get(inst, "state");
    CHECK_EQ(get(inst, "state", 20), std::string("<-1>"));
    CHECK_EQ(get(inst, "state", (int)now.size()), std::string("<-1>"));       /* no room for NUL */
    CHECK_EQ(get(inst, "state", (int)now.size() + 1), now);

    /* --- nodes without a live handle are left out --- */
    inst.graph.nodes[1].handle = nullptr;
    std::string partial = get(inst, "state");
    CHECK(partial.find("\"3_root\"") == std::string::npos);
    CHECK(partial.find("\"0_root\"") != std::string::npos);
    rnbo_set_param(&inst, "state", state.c_str());  /* the dead node's keys are skipped */
    CHECK_EQ(b.values[0], 10.0);

    /* --- no graph at all still answers with the volume --- */
    RnboInstance bare{};
    bare.volume = 0.5f;
    CHECK_EQ(get(bare, "state"), std::string("{\"volume\":0.5}"));

    if (g_failures) { std::cerr << g_failures << " check(s) failed\n"; return 1; }
    std::cout << "ok\n";
    return 0;
}
