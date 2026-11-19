# SNESticle Aurora — project instructions

Keep existing source licensing, build conventions and PS2 target compatibility. Inspect source, changelog and CI results before modifying or declaring a build successful.

## Universal concurrency policy
- Scope cancellation to superseded equivalent requests for the same task, project and ref. Independent diagnostics, jobs, manual requests, cores and publication/recovery tasks must not cancel one another. Use distinct task keys when adding workflows.
- Shared mutable state or release/tag publication must be serialized without cancellation; preserve exact explicit requests separately. The existing Continuous pipeline builds the complete application and recreates its release, so only obsolete pending requests can be superseded, not an active release. Verify CI before claiming success.
