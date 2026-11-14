// calib.wgsl - the fixed calibration kernel: a known, constant number of floating-point operations
// with no memory traffic worth speaking of, so its price says something about the silicon rather
// than about the scene, the resolution or the denoiser.
//
// Why it exists: `adapter.info` is the only identity a page is given, its fields are prototype
// getters that some browsers leave blank, and a run's milliseconds mean nothing until the card they
// came off is named. The filter price alone cannot settle it either - two different cards can land
// near each other on a memory-and-workgroup-bound pass. So every run also pays for exactly
//
//     THREADS * ITERATIONS * 2  =  1048576 * 1024 * 2  =  2147483648  FLOP
//
// of dependent fused multiply-adds, and logs the milliseconds and the derived GFLOP/s next to the
// adapter fields. The chain is dependent (each x feeds the next), so it cannot be reassociated or
// optimised away, and the result is stored so the compiler has to keep the loop.

struct Out { v : array<f32, 64> };

const THREADS    : u32 = 1048576u;   // 4096 workgroups x 256 threads
const ITERATIONS : u32 = 1024u;

@group(0) @binding(0) var<storage, read_write> out : Out;

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let i = gid.x + gid.y * 256u;
  var x : f32 = f32(i % 1024u) * 0.0009765625 + 0.5;
  let a : f32 = 1.0000001;
  let b : f32 = 0.0000001;
  for (var k : u32 = 0u; k < ITERATIONS; k = k + 1u) {
    x = fma(x, a, b);
  }
  out.v[i % 64u] = x;
}
