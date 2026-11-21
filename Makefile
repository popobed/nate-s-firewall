# Makefile - Empire d'Automatisation
#   Phase 5 (Niveaux 29 & 30) :
#     net_gateway            : O(1) frame ingestion, zero-copy actor routing
#     crypto_constant_time   : constant-time tenant/API-key verification
#   Phase 2 (Winsock) :
#     net_io                 : native async (non-blocking, polled) Winsock
#                               ingestion, zero-copy recv() straight into an
#                               ngw_mailbox_t slot's payload -- Windows-only,
#                               see include/net_io.h for why there is no
#                               POSIX path for this one module.
#   Phase 3 (actor architecture) :
#     ring_buffer             : generic lock-free MPSC ring, _Alignas(64)
#     actor_core              : isolated actor_t (private memory + mailbox)
#     msg_router              : O(1) tenant filtering, zero-copy dispatch,
#                               deterministic backpressure
#   Compact binary protocol (Option 1: no gRPC/Protobuf) :
#     wire_proto              : __attribute__((packed)) 64-byte wire_msg_t +
#                               its own lock-free ring, zero-copy-native by
#                               construction (portable, no Winsock dependency)
#     net_io (wire-mode)      : Winsock recv() straight into a wire_ring_t
#                               slot, no decode step -- see include/net_io.h
#   Signal acquisition (Niveaux 35, 37, 46 & 50 -- legitimate scope only;
#   see include/signal_core.h for what was deliberately NOT implemented) :
#     signal_core             : static PCM capture ring (SPSC), FIR
#                               decimation (explicit 4x unroll), a
#                               _Alignas(64) INT8 SRAM-style activation
#                               scratchpad with deterministic token
#                               decode, zero-copy emission into an
#                               actor_core.h mailbox, and real CPUID
#                               feature reporting (no code generation)
#   Phase 4 (Swarm d'agents metiers x Gateway reseau; see
#   include/swarm_dispatch.h for the "script_engine" honest-scope note) :
#     swarm_dispatch          : zero-copy net_gateway/net_io -> lead_parser
#                               -> agent_state (FSM) -> script_engine
#                               pipeline, O(1) tenant-indexed isolation,
#                               zero dynamic allocation
#   Phase 5 (audio; see include/audio_transcribe.h for the "transcribe"
#   honest-scope note) :
#     audio_transcribe        : PCM 16-bit frame FIR low-pass, a REAL
#                               AVX2 kernel (__attribute__((target("avx2"))),
#                               immintrin.h) gated behind a runtime CPUID
#                               check (reuses signal_core.h's
#                               sig_detect_cpu_features() -- this build
#                               machine reports avx2=0, so the portable
#                               scalar fallback is what actually runs
#                               here; both kernels are proven bit-identical
#                               in tests/test_audio_wasm_integration.c
#                               whenever AVX2 hardware IS present), zero-
#                               copy frame-index emission into an
#                               actor_core.h mailbox
#   Phase 6 (graphics; see include/wasm_gpu_engine.h for the "wasm" /
#   "Vulkan ou DirectX12" / "120 FPS" honest-scope notes -- Windows-only,
#   same posture as net_io.c) :
#     wasm_gpu_engine         : SoA vertex buffers (_Alignas(64) per
#                               channel, no false sharing), a draw-command
#                               queue built on the existing ring_buffer.h
#                               (not a second lock-free primitive), and a
#                               REAL Direct3D12 backend (D3D12CreateDevice
#                               + command queue + fence) -- Vulkan is not
#                               available in this toolchain (no SDK/loader
#                               installed), so D3D12 is used, exactly as
#                               the request's own "Vulkan ou DirectX12"
#                               phrasing allows
#   Phase 7 (Niveaux 11-20; see include/barry_storage.h for the full
#   honest-scope note against the ten external systems the request named) :
#     barry_storage           : static, zero-malloc KV cache + documentary
#                               store (lock-free open-addressed hash
#                               tables, seqlock-guarded in-place updates,
#                               automatic watermark-triggered shard
#                               activation), a single-field relation
#                               index, a bounded MPMC task queue/message
#                               broker (Vyukov ring, real <stdatomic.h>,
#                               strict O(1) backpressure), and a
#                               fixed-capacity per-metric time-series ring
#   Phase 8 (Niveaux 21-30; see include/brain_core.h for the full
#   honest-scope note against the ten systems the request named) :
#     brain_core               : IAM (ct_verify_token-based bearer auth +
#                               O(1) OAuth2-style scopes), a lock-free
#                               token-bucket rate limiter, a secrets
#                               vault (zero-residue revocation), a
#                               workflow/webhook orchestrator backed by
#                               barry_storage.h's MPMC queue, a cron
#                               hashed timing wheel driven by opaque
#                               cycle counts, per-tenant billing quotas
#                               in real SoA (_Alignas(64) per column,
#                               same pattern as infra_mesh.h's lb_pool_t),
#                               and a data-driven business rule engine
#                               with a minimal (NOT JSON/YAML) text loader
#   Phase 9 (Niveaux 31-40; see include/telemetry_fabric.h for the full
#   honest-scope note) :
#     telemetry_fabric        : an immutable, FNV-1a hash-chained WAL
#                               (git-commit-chain-style tamper evidence),
#                               a constant-time-verified (ct_equal_u64)
#                               per-tenant access-key store in real SoA,
#                               and additive audit-log coupling into
#                               infra_mesh.c (mesh_dispatch_audited) and
#                               brain_core.c (brain_billing_charge_audited)
#   Phase 10 (KARO overlay assistant surface; see include/karo_interface.h
#   for the full honest-scope note) :
#     karo_interface           : a lock-free CAS-rotated API key pool (no
#                               real key material compiled in -- see
#                               header), an owner-exclusive constant-time
#                               gate (ct_equal_u32) audited into
#                               telemetry_fabric.h, a thin bridge onto
#                               audio_transcribe.c's existing AVX2
#                               Goertzel pipeline, and a translucent
#                               overlay SoA submitted through
#                               wasm_gpu_engine.c's existing real D3D12
#                               fence backend -- none of those three
#                               reimplemented a second time
#   Phase 11 (Niveaux 41-50; see include/self_replicate.h for the full
#   honest-scope note) :
#     self_replicate           : per-actor sandbox health tracking with a
#                               deterministic threshold-triggered
#                               "predictive" auto-heal (real O(1)
#                               memory-erasure + mailbox drain, not a
#                               trained model), a true `static const`
#                               SaaS-template dictionary with a pure,
#                               deterministic generator, CPUID-adaptive
#                               kernel selection (sig_detect_cpu_features(),
#                               reused verbatim), and
#                               agn_execute_pure_replication() -- the
#                               orchestration capstone composing
#                               actor_core/infra_mesh/brain_core/
#                               telemetry_fabric/karo_interface, all
#                               reused, none reimplemented
#   Phase 12 (Niveaux 51-52; see include/signal_tracking.h for the full
#   honest-scope note) :
#     signal_tracking          : an I/Q noise-cleaning filter bank
#                               (single-pole IIR phase-noise smoothing +
#                               magnitude-based statistical outlier
#                               gating -- NOT true blind/Wiener
#                               deconvolution, no FFT exists in this
#                               codebase) and a Q21.10 fixed-point 3D
#                               kinetic trajectory tracker (velocity
#                               DERIVED from consecutive position
#                               reports, linear dead-reckoning
#                               extrapolation -- NOT a Kalman filter),
#                               real SoA (_Alignas(64) per column),
#                               standalone leaf module
#
# Strict build: -std=c11 -Wall -Wextra -Werror -O3 -pedantic
# Any compiler warning is a build failure by design. Zero dynamic
# allocation anywhere in src/ (every module embeds its own static
# storage) -- this Makefile is fully independent: no include/lib path
# reaches outside this project.

CC      ?= gcc
STD     := -std=c11
WARN    := -Wall -Wextra -Werror -pedantic
# -O3: standard aggressive optimization, portable across any target the
# compiler supports. -flto: whole-program link-time optimization, applied
# here (not only in `release` below) so it covers all 23 test binaries +
# karo_daemon + chrono-blackbox-cli, letting the linker inline/fuse across
# translation-unit boundaries (e.g. chrono_ingest.c <-> chrono_merkle.c
# <-> actor_core.c call sites) -- needs the matching -flto in LDFLAGS
# below to actually take effect at link time, not just at compile time.
# -fomit-frame-pointer: frees up one general-purpose register project-
# wide; safe, portable, no semantic or debuggability cost beyond slightly
# harder manual stack unwinding without DWARF info (which this project's
# own debug workflow does not rely on).
#
# Deliberately NOT here: -march=native and -fno-stack-protector.
# -march=native ties the produced binary to the exact CPU that compiled
# it (illegal-instruction crashes on any other machine) -- exactly what
# `make ALL`/`make test`'s own existing contract as "the portable,
# reproducible validation path" (see the `release` target below) rules
# out for the default build; it stays a release-only opt-in, unchanged.
# -fno-stack-protector removes stack-smashing canaries project-wide,
# including in net_io.c/net_gateway.c/wire_proto.c -- the modules that
# parse untrusted bytes straight off a socket. That is a real security
# trade-off (a meaningful defense-in-depth layer against exactly the bug
# class this project's own crypto_constant_time.h/CWE-457 hardening notes
# elsewhere take seriously), not a free performance win, so it is not
# applied here without that being an explicit, separate decision.
OPT     := -O3 -flto -fomit-frame-pointer
INCLUDE := -Iinclude

CFLAGS  := $(STD) $(WARN) $(OPT) $(INCLUDE)
LDFLAGS := -flto

# `make CC=clang clean test` validates the same strict build with LLVM's
# clang instead of gcc (verified: clang 22 compiles every module here --
# COBJMACROS/INITGUID D3D12 usage, stdatomic.h, __attribute__((target(...))
# AVX2 kernels, __attribute__((packed)) wire_msg_t -- with zero warnings
# under these exact flags). Plain `clang` on Windows defaults to the MSVC
# target triple, which does NOT understand this project's mingw-w64
# headers/libs; auto-add the mingw target so `CC=clang` just works without
# the caller needing to know that detail.
ifneq (,$(findstring clang,$(CC)))
    CFLAGS += --target=x86_64-w64-mingw32
endif

SRC_DIR   := src
TEST_DIR  := tests
BUILD_DIR := build

CORE_SRCS := $(SRC_DIR)/crypto_constant_time.c $(SRC_DIR)/net_gateway.c \
             $(SRC_DIR)/ring_buffer.c $(SRC_DIR)/actor_core.c $(SRC_DIR)/msg_router.c \
             $(SRC_DIR)/wire_proto.c $(SRC_DIR)/signal_core.c $(SRC_DIR)/swarm_dispatch.c \
             $(SRC_DIR)/audio_transcribe.c $(SRC_DIR)/barry_storage.c $(SRC_DIR)/brain_core.c \
             $(SRC_DIR)/telemetry_fabric.c $(SRC_DIR)/karo_interface.c $(SRC_DIR)/self_replicate.c \
             $(SRC_DIR)/signal_tracking.c $(SRC_DIR)/actor_pipeline.c $(SRC_DIR)/karo_ai_router.c \
             $(SRC_DIR)/agent_inspector.c $(SRC_DIR)/agent_runner.c $(SRC_DIR)/sha256.c \
             $(SRC_DIR)/rate_limit.c $(SRC_DIR)/chrono_tick.c

TEST_CRYPTO_BIN     := $(BUILD_DIR)/test_crypto_constant_time
TEST_GATEWAY_BIN    := $(BUILD_DIR)/test_net_gateway
TEST_NETIO_BIN      := $(BUILD_DIR)/test_net_io
TEST_KARO_NETWORK_BIN := $(BUILD_DIR)/test_karo_network
TEST_CONCURRENT_BIN := $(BUILD_DIR)/test_concurrent_core
TEST_WIREPROTO_BIN  := $(BUILD_DIR)/test_wire_proto
TEST_SIGNAL_BIN     := $(BUILD_DIR)/test_signal_core
TEST_SWARM_BIN      := $(BUILD_DIR)/test_swarm_integration
TEST_AUDIO_WASM_BIN := $(BUILD_DIR)/test_audio_wasm_integration
TEST_INFRA_MESH_BIN := $(BUILD_DIR)/test_infra_mesh
TEST_BARRY_BIN       := $(BUILD_DIR)/test_barry_storage
TEST_BRAIN_BIN        := $(BUILD_DIR)/test_brain_core
TEST_TELEMETRY_BIN     := $(BUILD_DIR)/test_telemetry_fabric
TEST_KARO_BIN           := $(BUILD_DIR)/test_karo_core
TEST_SELF_REPLICATE_BIN := $(BUILD_DIR)/test_self_replicate
TEST_SIGNAL_TRACKING_BIN := $(BUILD_DIR)/test_signal_tracking
TEST_ACTOR_PIPELINE_BIN := $(BUILD_DIR)/test_actor_pipeline
TEST_AGENT_INSPECTOR_BIN := $(BUILD_DIR)/test_agent_inspector
TEST_AGENT_RUNNER_BIN := $(BUILD_DIR)/test_agent_runner
TEST_CHRONO_INGEST_BIN := $(BUILD_DIR)/test_chrono_ingest
TEST_CHRONO_BLOCK_LINK_BIN := $(BUILD_DIR)/test_chrono_block_link
TEST_CHRONO_MERKLE_STORAGE_BIN := $(BUILD_DIR)/test_chrono_merkle_storage
TEST_CHRONO_STRESS_BIN := $(BUILD_DIR)/test_chrono_stress
TEST_ACTOR_RATE_TICK_BIN := $(BUILD_DIR)/test_actor_rate_tick

# karo_daemon: the standalone runnable entry point (src/karo_daemon.c) --
# Windows-only, same posture as net_io.c/wasm_gpu_engine.c/infra_mesh.c
# above (RegisterHotKey + karo_interface.h's D3D12 overlay backend). Not
# part of $(TEST_BINS)/`test` -- it is a real process, not a check-based
# test binary; built via `make ALL` or `make release`, see below.
KARO_DAEMON_BIN := $(BUILD_DIR)/karo_daemon

# chrono-blackbox-cli: standalone offline audit tool (src/chrono_cli.c) --
# never opens a socket, never calls chrono_ingest_init(). Same posture as
# karo_daemon: a real runnable process, not a check-based test binary, so
# not part of $(TEST_BINS)/`test` -- built via `make ALL` or directly.
CHRONO_CLI_BIN := $(BUILD_DIR)/chrono-blackbox-cli

ifeq ($(OS),Windows_NT)
    BIN_EXT := .exe

    # net_io.c: Winsock (winsock2.h/ws2tcpip.h) needs explicit linkage
    # against ws2_32 -- unlike kernel32 (CreateThread et al., used by the
    # test harnesses), it is not linked by default. Several test binaries
    # also need it (their stress harnesses use CreateThread, or they link
    # net_io's object, which references Winsock symbols).
    LDFLAGS += -lws2_32

    # net_io is Winsock-specific by design (see include/net_io.h) -- only
    # built/tested on Windows. On any other OS it is simply not part of
    # the build (net_io.h itself would #error if force-included there).
    CORE_SRCS += $(SRC_DIR)/net_io.c

    # chrono_ingest.c (CHRONO-BLACKBOX async log ingestion) wraps
    # net_io.c's wire-mode server -- same Windows-only posture, for the
    # same reason (see include/chrono_ingest.h).
    CORE_SRCS += $(SRC_DIR)/chrono_ingest.c

    # chrono_merkle.c is portable in principle (pure SHA-256 over
    # chrono_block_t) but depends on chrono_ingest.h, which is
    # Windows-only -- built here for the same reason.
    CORE_SRCS += $(SRC_DIR)/chrono_merkle.c

    # chrono_storage.c's flush path uses Win32 CreateFileA(...,
    # FILE_APPEND_DATA, ...) directly (see include/chrono_storage.h) --
    # Windows-only for the same reason as net_io.c/chrono_ingest.c.
    CORE_SRCS += $(SRC_DIR)/chrono_storage.c

    # wasm_gpu_engine is Direct3D12-specific by design (see
    # include/wasm_gpu_engine.h) -- same Windows-only posture as net_io.c.
    # -ld3d12 links against the real Direct3D12 import library (mingw-w64
    # ships d3d12.h/libd3d12.a; there is no Vulkan SDK/loader in this
    # toolchain, see the header's honest-scope note for why D3D12 was
    # chosen over Vulkan).
    LDFLAGS += -ld3d12
    CORE_SRCS += $(SRC_DIR)/wasm_gpu_engine.c

    # infra_mesh.c's local block store uses the Windows CRT low-level fd
    # calls (_open/_write/_read/_close, io.h) -- same Windows-only posture
    # as net_io.c and wasm_gpu_engine.c, for the same reason (no portable
    # abstraction attempted where this project only ever targets one OS).
    CORE_SRCS += $(SRC_DIR)/infra_mesh.c

    # karo_screen_capture.c: real D3D11 Desktop Duplication (see that
    # header's honest scope) -- needs libd3d11.a beyond the D3D12 import
    # lib above (different API family, both mingw-w64-provided).
    LDFLAGS += -ld3d11
    CORE_SRCS += $(SRC_DIR)/karo_screen_capture.c

    # karo_window.c: the Win32 overlay window (RegisterClassEx/CreateWindowEx/
    # child controls) -- Windows-only by construction, same posture as the
    # other files in this block.
    CORE_SRCS += $(SRC_DIR)/karo_window.c

    # karo_bridge.c: file-based IPC to the separate karo_vision_bridge.py
    # process (CreateDirectoryA) -- Windows-only, same posture as the rest
    # of this block.
    CORE_SRCS += $(SRC_DIR)/karo_bridge.c

    TEST_BINS := $(TEST_CRYPTO_BIN)$(BIN_EXT) $(TEST_GATEWAY_BIN)$(BIN_EXT) \
                 $(TEST_NETIO_BIN)$(BIN_EXT) $(TEST_KARO_NETWORK_BIN)$(BIN_EXT) $(TEST_CONCURRENT_BIN)$(BIN_EXT) \
                 $(TEST_WIREPROTO_BIN)$(BIN_EXT) $(TEST_SIGNAL_BIN)$(BIN_EXT) \
                 $(TEST_SWARM_BIN)$(BIN_EXT) $(TEST_AUDIO_WASM_BIN)$(BIN_EXT) \
                 $(TEST_INFRA_MESH_BIN)$(BIN_EXT) $(TEST_BARRY_BIN)$(BIN_EXT) \
                 $(TEST_BRAIN_BIN)$(BIN_EXT) $(TEST_TELEMETRY_BIN)$(BIN_EXT) $(TEST_KARO_BIN)$(BIN_EXT) \
                 $(TEST_SELF_REPLICATE_BIN)$(BIN_EXT) $(TEST_SIGNAL_TRACKING_BIN)$(BIN_EXT) \
                 $(TEST_ACTOR_PIPELINE_BIN)$(BIN_EXT) \
                 $(TEST_AGENT_INSPECTOR_BIN)$(BIN_EXT) $(TEST_AGENT_RUNNER_BIN)$(BIN_EXT) \
                 $(TEST_CHRONO_INGEST_BIN)$(BIN_EXT) $(TEST_CHRONO_BLOCK_LINK_BIN)$(BIN_EXT) \
                 $(TEST_CHRONO_MERKLE_STORAGE_BIN)$(BIN_EXT) $(TEST_CHRONO_STRESS_BIN)$(BIN_EXT) \
                 $(TEST_ACTOR_RATE_TICK_BIN)$(BIN_EXT)
else
    CFLAGS  += -pthread
    LDFLAGS += -pthread
    BIN_EXT :=
    TEST_BINS := $(TEST_CRYPTO_BIN)$(BIN_EXT) $(TEST_GATEWAY_BIN)$(BIN_EXT) \
                 $(TEST_CONCURRENT_BIN)$(BIN_EXT) $(TEST_WIREPROTO_BIN)$(BIN_EXT) \
                 $(TEST_SIGNAL_BIN)$(BIN_EXT) $(TEST_SWARM_BIN)$(BIN_EXT) \
                 $(TEST_BARRY_BIN)$(BIN_EXT) $(TEST_BRAIN_BIN)$(BIN_EXT) \
                 $(TEST_TELEMETRY_BIN)$(BIN_EXT) $(TEST_KARO_BIN)$(BIN_EXT) \
                 $(TEST_SELF_REPLICATE_BIN)$(BIN_EXT) $(TEST_SIGNAL_TRACKING_BIN)$(BIN_EXT) \
                 $(TEST_ACTOR_PIPELINE_BIN)$(BIN_EXT) \
                 $(TEST_AGENT_INSPECTOR_BIN)$(BIN_EXT) $(TEST_AGENT_RUNNER_BIN)$(BIN_EXT)
endif

CORE_OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(CORE_SRCS))

# --------------------------------------------------------------------
# wasm-demo (OPTIONAL, not part of `all`/`test`): verifies -- for real,
# not a claim -- that the PORTABLE subset of wasm_gpu_engine.h/.c (the
# SoA vertex buffer + draw-command queue, zero Windows/D3D12 dependency)
# actually compiles to WebAssembly object code. See wasm_gpu_engine.h's
# "Ce qui compile REELLEMENT pour WebAssembly" note for exactly what this
# does and does not prove: this produces .wasm OBJECT files (memcpy/memset
# left as ordinary unresolved externals, see tools/wasm_shim/string.h),
# not a linked/runnable .wasm module -- that needs a real libc for wasm32
# (wasi-libc/Emscripten), which is not installed in this toolchain.
# Requires clang with the wasm32 backend (verified present: LLVM 22).
# --------------------------------------------------------------------
WASM_CC      := clang
WASM_CFLAGS  := -std=c11 -Wall -Wextra -Werror -O3 -pedantic --target=wasm32 -ffreestanding \
                -idirafter tools/wasm_shim $(INCLUDE)
WASM_BUILD_DIR := $(BUILD_DIR)/wasm-demo

.PHONY: all test clean wasm-demo ALL release

wasm-demo:
	mkdir -p $(WASM_BUILD_DIR)
	$(WASM_CC) $(WASM_CFLAGS) -c $(SRC_DIR)/ring_buffer.c -o $(WASM_BUILD_DIR)/ring_buffer.wasm.o
	$(WASM_CC) $(WASM_CFLAGS) -c $(SRC_DIR)/wasm_gpu_engine.c -o $(WASM_BUILD_DIR)/wasm_gpu_engine.wasm.o
	@echo "wasm-demo: portable SoA/command-queue module compiled to real WebAssembly object code:"
	@ls -la $(WASM_BUILD_DIR)/*.wasm.o

all: $(TEST_BINS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/crypto_constant_time.o: $(SRC_DIR)/crypto_constant_time.c include/crypto_constant_time.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/net_gateway.o: $(SRC_DIR)/net_gateway.c include/net_gateway.h include/crypto_constant_time.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/net_io.o: $(SRC_DIR)/net_io.c include/net_io.h include/net_gateway.h include/wire_proto.h include/rate_limit.h include/telemetry_fabric.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/chrono_ingest.o: $(SRC_DIR)/chrono_ingest.c include/chrono_ingest.h include/net_io.h include/wire_proto.h include/barry_storage.h include/sha256.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/sha256.o: $(SRC_DIR)/sha256.c include/sha256.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/chrono_merkle.o: $(SRC_DIR)/chrono_merkle.c include/chrono_merkle.h include/chrono_ingest.h include/sha256.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/chrono_storage.o: $(SRC_DIR)/chrono_storage.c include/chrono_storage.h include/chrono_merkle.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/ring_buffer.o: $(SRC_DIR)/ring_buffer.c include/ring_buffer.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rate_limit.o: $(SRC_DIR)/rate_limit.c include/rate_limit.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/chrono_tick.o: $(SRC_DIR)/chrono_tick.c include/chrono_tick.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/actor_core.o: $(SRC_DIR)/actor_core.c include/actor_core.h include/ring_buffer.h include/rate_limit.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/msg_router.o: $(SRC_DIR)/msg_router.c include/msg_router.h include/actor_core.h include/ring_buffer.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/wire_proto.o: $(SRC_DIR)/wire_proto.c include/wire_proto.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/signal_core.o: $(SRC_DIR)/signal_core.c include/signal_core.h include/actor_core.h include/ring_buffer.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/swarm_dispatch.o: $(SRC_DIR)/swarm_dispatch.c include/swarm_dispatch.h include/net_gateway.h include/crypto_constant_time.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/audio_transcribe.o: $(SRC_DIR)/audio_transcribe.c include/audio_transcribe.h include/signal_core.h include/actor_core.h include/ring_buffer.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/wasm_gpu_engine.o: $(SRC_DIR)/wasm_gpu_engine.c include/wasm_gpu_engine.h include/ring_buffer.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/infra_mesh.o: $(SRC_DIR)/infra_mesh.c include/infra_mesh.h include/actor_core.h include/wire_proto.h include/crypto_constant_time.h include/barry_storage.h include/telemetry_fabric.h include/signal_tracking.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/barry_storage.o: $(SRC_DIR)/barry_storage.c include/barry_storage.h include/wire_proto.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/brain_core.o: $(SRC_DIR)/brain_core.c include/brain_core.h include/crypto_constant_time.h include/barry_storage.h include/telemetry_fabric.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/telemetry_fabric.o: $(SRC_DIR)/telemetry_fabric.c include/telemetry_fabric.h include/crypto_constant_time.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/karo_interface.o: $(SRC_DIR)/karo_interface.c include/karo_interface.h include/crypto_constant_time.h include/telemetry_fabric.h include/audio_transcribe.h include/ring_buffer.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/self_replicate.o: $(SRC_DIR)/self_replicate.c include/self_replicate.h include/actor_core.h include/infra_mesh.h include/brain_core.h include/telemetry_fabric.h include/karo_interface.h include/signal_core.h include/signal_tracking.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/signal_tracking.o: $(SRC_DIR)/signal_tracking.c include/signal_tracking.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/actor_pipeline.o: $(SRC_DIR)/actor_pipeline.c include/actor_pipeline.h include/actor_core.h include/barry_storage.h include/telemetry_fabric.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/agent_inspector.o: $(SRC_DIR)/agent_inspector.c include/agent_inspector.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/agent_runner.o: $(SRC_DIR)/agent_runner.c include/agent_runner.h include/agent_inspector.h include/telemetry_fabric.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/karo_ai_router.o: $(SRC_DIR)/karo_ai_router.c include/karo_ai_router.h include/karo_interface.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/karo_screen_capture.o: $(SRC_DIR)/karo_screen_capture.c include/karo_screen_capture.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/karo_window.o: $(SRC_DIR)/karo_window.c include/karo_window.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/karo_bridge.o: $(SRC_DIR)/karo_bridge.c include/karo_bridge.h include/karo_screen_capture.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TEST_CRYPTO_BIN)$(BIN_EXT): $(TEST_DIR)/test_crypto_constant_time.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_crypto_constant_time.c $(CORE_OBJS) -o $@ $(LDFLAGS)

$(TEST_GATEWAY_BIN)$(BIN_EXT): $(TEST_DIR)/test_net_gateway.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_net_gateway.c $(CORE_OBJS) -o $@ $(LDFLAGS)

$(TEST_NETIO_BIN)$(BIN_EXT): $(TEST_DIR)/test_net_io.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_net_io.c $(CORE_OBJS) -o $@ $(LDFLAGS)

$(TEST_KARO_NETWORK_BIN)$(BIN_EXT): $(TEST_DIR)/test_karo_network.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_karo_network.c $(CORE_OBJS) -o $@ $(LDFLAGS)

$(TEST_CONCURRENT_BIN)$(BIN_EXT): $(TEST_DIR)/test_concurrent_core.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_concurrent_core.c $(CORE_OBJS) -o $@ $(LDFLAGS)

$(TEST_WIREPROTO_BIN)$(BIN_EXT): $(TEST_DIR)/test_wire_proto.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_wire_proto.c $(CORE_OBJS) -o $@ $(LDFLAGS)

$(TEST_SIGNAL_BIN)$(BIN_EXT): $(TEST_DIR)/test_signal_core.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_signal_core.c $(CORE_OBJS) -o $@ $(LDFLAGS)

$(TEST_SWARM_BIN)$(BIN_EXT): $(TEST_DIR)/test_swarm_integration.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_swarm_integration.c $(CORE_OBJS) -o $@ $(LDFLAGS)

# test_audio_wasm_integration.c includes wasm_gpu_engine.h (Direct3D12,
# Windows-only) -- this rule is only ever reached via $(TEST_BINS) on
# Windows (see the ifeq ($(OS),Windows_NT) branch above).
$(TEST_AUDIO_WASM_BIN)$(BIN_EXT): $(TEST_DIR)/test_audio_wasm_integration.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_audio_wasm_integration.c $(CORE_OBJS) -o $@ $(LDFLAGS)

# test_infra_mesh.c uses infra_mesh.c's local block store (Windows CRT
# low-level fd calls) -- this rule is only ever reached via $(TEST_BINS)
# on Windows (see the ifeq ($(OS),Windows_NT) branch above).
$(TEST_INFRA_MESH_BIN)$(BIN_EXT): $(TEST_DIR)/test_infra_mesh.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_infra_mesh.c $(CORE_OBJS) -o $@ $(LDFLAGS)

# test_barry_storage.c is portable (#if _WIN32 CreateThread / else pthread,
# same pattern as test_concurrent_core.c) -- built/run on every OS, not
# gated behind the Windows-only ifeq branch above.
$(TEST_BARRY_BIN)$(BIN_EXT): $(TEST_DIR)/test_barry_storage.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_barry_storage.c $(CORE_OBJS) -o $@ $(LDFLAGS)

# test_brain_core.c is portable (same #if _WIN32 CreateThread / else
# pthread pattern as test_concurrent_core.c / test_barry_storage.c).
$(TEST_BRAIN_BIN)$(BIN_EXT): $(TEST_DIR)/test_brain_core.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_brain_core.c $(CORE_OBJS) -o $@ $(LDFLAGS)

# test_telemetry_fabric.c is portable (same threading pattern as above).
$(TEST_TELEMETRY_BIN)$(BIN_EXT): $(TEST_DIR)/test_telemetry_fabric.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_telemetry_fabric.c $(CORE_OBJS) -o $@ $(LDFLAGS)

# test_karo_core.c is portable overall; its D3D12 overlay-submit checks are
# internally guarded with #if defined(_WIN32), same posture as
# karo_interface.h/.c's own Part D.
$(TEST_KARO_BIN)$(BIN_EXT): $(TEST_DIR)/test_karo_core.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_karo_core.c $(CORE_OBJS) -o $@ $(LDFLAGS)

# test_self_replicate.c is portable (same threading pattern as above).
$(TEST_SELF_REPLICATE_BIN)$(BIN_EXT): $(TEST_DIR)/test_self_replicate.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_self_replicate.c $(CORE_OBJS) -o $@ $(LDFLAGS)

# test_signal_tracking.c is portable (same threading pattern as above).
$(TEST_SIGNAL_TRACKING_BIN)$(BIN_EXT): $(TEST_DIR)/test_signal_tracking.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_signal_tracking.c $(CORE_OBJS) -o $@ $(LDFLAGS)

# test_actor_pipeline.c is portable (same #if _WIN32 CreateThread / else
# pthread pattern as test_barry_storage.c/test_telemetry_fabric.c).
$(TEST_ACTOR_PIPELINE_BIN)$(BIN_EXT): $(TEST_DIR)/test_actor_pipeline.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_actor_pipeline.c $(CORE_OBJS) -o $@ $(LDFLAGS)

# test_agent_inspector.c / test_agent_runner.c are fully portable (no
# threading, no platform API beyond agent_runner.c's own #if _WIN32
# GetTickCount64() / else clock_gettime() monotonic-clock split).
$(TEST_AGENT_INSPECTOR_BIN)$(BIN_EXT): $(TEST_DIR)/test_agent_inspector.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_agent_inspector.c $(CORE_OBJS) -o $@ $(LDFLAGS)

$(TEST_AGENT_RUNNER_BIN)$(BIN_EXT): $(TEST_DIR)/test_agent_runner.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_agent_runner.c $(CORE_OBJS) -o $@ $(LDFLAGS)

# CHRONO-BLACKBOX async log ingestion -- wraps net_io.c's wire-mode
# server, Windows-only for the same reason (see include/chrono_ingest.h);
# this rule is only ever reached via $(TEST_BINS) on Windows.
$(TEST_CHRONO_INGEST_BIN)$(BIN_EXT): $(TEST_DIR)/test_chrono_ingest.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_chrono_ingest.c $(CORE_OBJS) -o $@ $(LDFLAGS)

# Block-linking (SHA-256-sealed hash chain on top of chrono_ingest.c) --
# same Windows-only posture as test_chrono_ingest.c above (it includes
# chrono_ingest.h); this rule is only ever reached via $(TEST_BINS) on
# Windows.
$(TEST_CHRONO_BLOCK_LINK_BIN)$(BIN_EXT): $(TEST_DIR)/test_chrono_block_link.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_chrono_block_link.c $(CORE_OBJS) -o $@ $(LDFLAGS)

# Merkle root + append-only storage on top of chrono_ingest.c's sealed
# chain -- same Windows-only posture as the two chrono_* test binaries
# above; this rule is only ever reached via $(TEST_BINS) on Windows.
$(TEST_CHRONO_MERKLE_STORAGE_BIN)$(BIN_EXT): $(TEST_DIR)/test_chrono_merkle_storage.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_chrono_merkle_storage.c $(CORE_OBJS) -o $@ $(LDFLAGS)

# Concurrent load bench (8 client threads x 1000 frames against the real
# reactor) -- same Windows-only posture as the other chrono_* test
# binaries above; this rule is only ever reached via $(TEST_BINS) on
# Windows.
$(TEST_CHRONO_STRESS_BIN)$(BIN_EXT): $(TEST_DIR)/test_chrono_stress.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_chrono_stress.c $(CORE_OBJS) -o $@ $(LDFLAGS)

# actor_core.c's private-heap/rate-limit extension + chrono_tick.c --
# uses the Win32 thread API directly for its concurrency stress sections,
# same posture as the other CreateThread-based test binaries above; this
# rule is only ever reached via $(TEST_BINS) on Windows.
$(TEST_ACTOR_RATE_TICK_BIN)$(BIN_EXT): $(TEST_DIR)/test_actor_rate_tick.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/test_actor_rate_tick.c $(CORE_OBJS) -o $@ $(LDFLAGS)

test: $(TEST_BINS)
	./$(TEST_CRYPTO_BIN)$(BIN_EXT)
	./$(TEST_GATEWAY_BIN)$(BIN_EXT)
ifeq ($(OS),Windows_NT)
	./$(TEST_NETIO_BIN)$(BIN_EXT)
	./$(TEST_KARO_NETWORK_BIN)$(BIN_EXT)
endif
	./$(TEST_CONCURRENT_BIN)$(BIN_EXT)
	./$(TEST_WIREPROTO_BIN)$(BIN_EXT)
	./$(TEST_SIGNAL_BIN)$(BIN_EXT)
	./$(TEST_SWARM_BIN)$(BIN_EXT)
ifeq ($(OS),Windows_NT)
	./$(TEST_AUDIO_WASM_BIN)$(BIN_EXT)
	./$(TEST_INFRA_MESH_BIN)$(BIN_EXT)
endif
	./$(TEST_BARRY_BIN)$(BIN_EXT)
	./$(TEST_BRAIN_BIN)$(BIN_EXT)
	./$(TEST_TELEMETRY_BIN)$(BIN_EXT)
	./$(TEST_KARO_BIN)$(BIN_EXT)
	./$(TEST_SELF_REPLICATE_BIN)$(BIN_EXT)
	./$(TEST_SIGNAL_TRACKING_BIN)$(BIN_EXT)
	./$(TEST_ACTOR_PIPELINE_BIN)$(BIN_EXT)
	./$(TEST_AGENT_INSPECTOR_BIN)$(BIN_EXT)
	./$(TEST_AGENT_RUNNER_BIN)$(BIN_EXT)
ifeq ($(OS),Windows_NT)
	./$(TEST_CHRONO_INGEST_BIN)$(BIN_EXT)
	./$(TEST_CHRONO_BLOCK_LINK_BIN)$(BIN_EXT)
	./$(TEST_CHRONO_MERKLE_STORAGE_BIN)$(BIN_EXT)
	./$(TEST_CHRONO_STRESS_BIN)$(BIN_EXT)
	./$(TEST_ACTOR_RATE_TICK_BIN)$(BIN_EXT)
endif

clean:
	rm -rf $(BUILD_DIR)

# --------------------------------------------------------------------
# karo_daemon + ALL/release: see src/karo_daemon.c's own honesty notes for
# exactly what "ecoute les evenements systeme" does (ONE named
# RegisterHotKey combination, never a keystroke hook) and does not (no
# live WASAPI microphone/screen capture) mean here. `make ALL` = `make
# all` (every test binary) plus karo_daemon, built with this project's
# normal strict flags (which now include -flto/-fomit-frame-pointer, see
# CFLAGS/OPT above -- portable, reproducible, no CPU-specific codegen).
# `make release` rebuilds ONLY karo_daemon with -march=native layered on
# top of those same strict flags, for a locally-tuned build -- not a
# substitute for `make ALL`/`make test`, which remain the portable,
# reproducible validation path.
# --------------------------------------------------------------------
ifeq ($(OS),Windows_NT)
$(KARO_DAEMON_BIN)$(BIN_EXT): $(SRC_DIR)/karo_daemon.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC_DIR)/karo_daemon.c $(CORE_OBJS) -o $@ $(LDFLAGS) -luser32

$(CHRONO_CLI_BIN)$(BIN_EXT): $(SRC_DIR)/chrono_cli.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC_DIR)/chrono_cli.c $(CORE_OBJS) -o $@ $(LDFLAGS)

ALL: all $(KARO_DAEMON_BIN)$(BIN_EXT) $(CHRONO_CLI_BIN)$(BIN_EXT)

release: CFLAGS += -march=native
release: $(KARO_DAEMON_BIN)$(BIN_EXT)
else
ALL: all
	@echo "karo_daemon is Windows-only (RegisterHotKey + D3D12 overlay backend) -- skipped on this OS."

release:
	@echo "karo_daemon release build is Windows-only -- skipped on this OS."
endif
