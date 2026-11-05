const std = @import("std");

pub fn build(b: *std.Build) void {
    const mod = b.createModule(.{
        .target = b.standardTargetOptions(.{}),
        .optimize = b.standardOptimizeOption(.{}),
        .link_libc = true,
    });
    mod.addCSourceFiles(.{ .root = b.path("src"), .files = &.{"main.c"} });

    // On macOS only the static libghostty-vt is an artifact. SDL2_ttf without
    // pkg-config: it would link SDL2 a second time, and dyld refuses that.
    if (b.lazyDependency("ghostty", .{})) |dep|
        mod.linkLibrary(dep.artifact("ghostty-vt-static"));
    mod.linkSystemLibrary("SDL2", .{});
    mod.linkSystemLibrary("SDL2_ttf", .{ .use_pkg_config = .no });

    const exe = b.addExecutable(.{ .name = "term", .root_module = mod });
    b.installArtifact(exe);

    const run = b.addRunArtifact(exe);
    run.step.dependOn(b.getInstallStep());
    b.step("run", "Run the terminal").dependOn(&run.step);
}
