namespace cpp2 press_test

struct Request {
    1: bool echo_attachment;
    2: binary payload;
    3: i32 int_val;
    4: i64 long_val;
    5: double double_val;
    6: string str_val;
    7: list<i32> int_list;
    8: list<string> str_list;
    9: set<string> str_set;
    10: map<string, i64> kv_map;
}

struct PerfTestResponse {
    1: double cpu_usage;
    2: double mem_usage;
    3: binary payload;
}

service PerfTestService {
    PerfTestResponse test(1: Request req);
    // Same work as test(), run on the IO thread (no IO->CPU handoff); the
    // no-handoff reference point for the experiments.
    PerfTestResponse testEb(1: Request req) (thread = 'eb');
}
