// empty.wgsl - the calibration dispatch. Nothing but a kernel that does nothing, so the cost of the
// timing harness itself (submit + queue.onSubmittedWorkDone(), measured with performance.now() from
// web content, which has no GPU timestamps it can be trusted to use) can be subtracted from every other
// number, or at least named.
//
// Two entry points, because there are two different floors worth stating:
//   main      - one thread, one workgroup: the pure CPU<->GPU round trip per submission, as the probe
//               measured it (0.167 - 1.000 ms per dispatch on this machine, bimodally).
//   mainGrid  - the same workgroup grid a real full-frame pass launches (w/8 x h/8 groups of 8x8) with
//               every thread doing nothing: what launching a pass at that size costs before any work.

@compute @workgroup_size(1)
fn main() {}

@compute @workgroup_size(8, 8)
fn mainGrid() {}
