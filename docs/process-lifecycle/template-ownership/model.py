"""Executable heap ownership specification. Page numbers are absolute Sv39 VPNs."""
import json
import random
from collections import Counter


WIDTH = 512


class Template:
    def __init__(self, values, policy):
        self.policy = policy
        self.tables = {}
        self.frames = {}
        self.refs = Counter()
        self.ops = Counter()
        self.handles = 1
        self.open = True
        self.next_id = 1
        for vpn, value in values.items():
            table, slot = divmod(vpn, WIDTH)
            if table not in self.tables:
                self.tables[table] = [None] * WIDTH
            self.tables[table][slot] = self.alloc(value)
        self.tables = {k: tuple(v) for k, v in self.tables.items()}

    def alloc(self, value):
        frame = self.next_id
        self.next_id += 1
        self.frames[frame] = value
        self.refs[frame] = 1
        return frame

    def drop(self, frame):
        assert self.refs[frame] > 0
        self.refs[frame] -= 1
        if not self.refs[frame]:
            del self.refs[frame], self.frames[frame]

    def release(self):
        self.handles -= 1
        if not self.handles:
            for table in self.tables.values():
                for frame in table:
                    if frame is not None:
                        self.drop(frame)
                        self.ops["eviction_drops"] += 1
            self.tables.clear()

    def close(self):
        assert self.open
        self.open = False
        self.release()

    def spawn(self):
        if not self.open:
            raise ValueError("closed")
        self.handles += 1
        return Branch(self)


class Branch:
    def __init__(self, t):
        self.t = t
        self.tables = dict(t.tables)
        self.owned = {}
        if 0 in self.tables:
            self.split(0)  # Fixed image entries require a private first table.

    def split(self, key):
        self.tables[key] = list(self.tables[key])
        self.t.ops["pte_slots_copied"] += WIDTH
        if self.t.policy == "table_refs":
            for frame in self.tables[key]:
                if frame is not None:
                    self.t.refs[frame] += 1
                    self.t.ops["borrow_retain"] += 1

    def frame(self, vpn):
        key, slot = divmod(vpn, WIDTH)
        return self.tables[key][slot]

    def read(self, vpn):
        frame = self.frame(vpn)
        return None if frame is None else self.t.frames[frame]

    def edit(self, changes, fail_at=None):
        # One transaction, distinct valid slots. None means unmap.
        staged = {}
        split_keys = set()
        allocated = 0

        def reservation():
            nonlocal allocated
            allocated += 1
            if allocated == fail_at:
                raise MemoryError

        try:
            for vpn, value in changes.items():
                key = vpn // WIDTH
                if self.frame(vpn) is None:
                    raise KeyError(vpn)
                if isinstance(self.tables[key], tuple) and key not in split_keys:
                    reservation()
                    split_keys.add(key)
                if value is not None and vpn not in self.owned:
                    reservation()
                    staged[vpn] = self.t.alloc(value)
        except (MemoryError, KeyError):
            for frame in staged.values():
                self.t.drop(frame)
            raise

        for key in split_keys:
            self.split(key)
        for vpn, value in changes.items():
            key, slot = divmod(vpn, WIDTH)
            old = self.frame(vpn)
            if vpn in self.owned and value is not None:
                self.t.frames[old] = value
                continue
            if vpn in self.owned:
                self.t.drop(self.owned.pop(vpn))
            elif self.t.policy == "table_refs":
                self.t.drop(old)
                self.t.ops["borrow_drop"] += 1
            frame = staged.get(vpn)
            self.tables[key][slot] = frame
            if frame is not None:
                self.owned[vpn] = frame

    def map_zero(self, vpn):
        key, slot = divmod(vpn, WIDTH)
        if key not in self.tables:
            self.tables[key] = [None] * WIDTH
        if self.frame(vpn) is not None:
            raise ValueError("occupied")
        if isinstance(self.tables[key], tuple):
            self.split(key)
        frame = self.t.alloc(0)
        self.tables[key][slot] = frame
        self.owned[vpn] = frame

    def exit(self):
        if self.t.policy == "table_refs":
            for table in self.tables.values():
                if isinstance(table, list):
                    self.t.ops["exit_slots_read"] += WIDTH
                    for frame in table:
                        if frame is not None:
                            self.t.drop(frame)
        else:
            for frame in self.owned.values():
                self.t.drop(frame)
                self.t.ops["exit_owned_drops"] += 1
        self.owned.clear()
        self.tables.clear()
        self.t.release()


def check(policy, seed):
    rng = random.Random(seed)
    pages = (3, 511, 512, 1023, 1024, 1536)
    initial = {vpn: vpn for vpn in pages}
    t = Template(initial, policy)
    branches = [t.spawn(), t.spawn()]
    oracle = [initial.copy(), initial.copy()]
    # Four reservations across two shared tables. Every prefix must roll back.
    for fault in range(1, 5):
        before = (t.refs.copy(), t.frames.copy(), dict(t.ops))
        try:
            branches[0].edit({1023: 17, 1024: 19}, fault)
        except MemoryError:
            pass
        else:
            raise AssertionError("missed allocation fault")
        assert before == (t.refs, t.frames, t.ops)
        assert all(isinstance(branches[0].tables[k], tuple) for k in (1, 2))
    t.close()
    try:
        t.spawn()
    except ValueError:
        pass
    else:
        raise AssertionError("closed template admitted another branch")
    for step in range(30):
        i = rng.randrange(2)
        vpn = rng.choice(pages)
        if oracle[i][vpn] is not None:
            value = None if rng.randrange(4) == 0 else rng.randrange(10000)
            branches[i].edit({vpn: value})
            oracle[i][vpn] = value
        elif rng.randrange(2):
            branches[i].map_zero(vpn)
            oracle[i][vpn] = 0
        for b, expected in zip(branches, oracle):
            assert [b.read(p) for p in pages] == list(expected.values())
    first = seed % 2
    branches[first].exit()
    assert [branches[1-first].read(p) for p in pages] == list(oracle[1-first].values())
    branches[1-first].exit()
    assert not t.refs and not t.frames and not t.tables and not t.handles


def run_case(policy, pages, writes, start=3):
    t = Template({vpn: vpn for vpn in range(start, start + pages)}, policy)
    branch = t.spawn()
    for vpn in writes:
        branch.edit({vpn: -vpn})
    branch.exit()
    t.close()
    assert not t.refs
    return dict(t.ops)


def repeated(policy, n):
    t = Template({vpn: vpn for vpn in range(3, 2051)}, policy)
    for _ in range(n):
        b = t.spawn()
        b.edit({512: 42})
        assert b.read(512) == 42
        b.exit()
        assert len(t.frames) == 2048 and t.handles == 1
    t.close()
    assert not t.frames and t.ops["eviction_drops"] == 2048
    return dict(t.ops)


def main():
    results = {}
    for policy in ("table_refs", "template_owner"):
        for seed in range(128):
            check(policy, seed)
        results[policy] = {
            name: run_case(policy, 2048, writes) for name, writes in {
                "readonly": [], "cluster4": range(3, 7),
                "spread4": range(3, 2051, 512), "full": range(3, 2051)
            }.items()
        }
        aligned = run_case(policy, 2048, [], start=512)
        assert aligned == {"eviction_drops": 2048}
        results[policy]["aligned_readonly"] = aligned
    for name in results["table_refs"]:
        a, b = (results[k][name] for k in results)
        assert a.get("pte_slots_copied", 0) == b.get("pte_slots_copied", 0)
        assert a["eviction_drops"] == b["eviction_drops"] == 2048
    print(json.dumps({"seeds_per_policy": 128, "heap_operation_counts": results,
                      "repeated_one_write": {p: {n: repeated(p, n) for n in (1, 8, 64)}
                                             for p in results}},
                     indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
