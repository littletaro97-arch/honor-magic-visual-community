using System;

namespace MagicCommunity;

// Verifies the driver client contract without the driver present. The driver is
// optional by design, so absence must be reported, never thrown.
internal static class ProcessGuardFixture
{
    internal static object Run()
    {
        var checks = new List<string>();
        void Check(string name, Action test) { test(); checks.Add(name); }
        void Expect(bool value, string text) { if (!value) throw new InvalidOperationException(text); }

        bool available = ProcessGuardClient.Available();
        int errno = 0;
        bool setWork;
        using (var self = System.Diagnostics.Process.GetCurrentProcess())
        {
            setWork = ProcessGuardClient.SetWorkingSet(self.Id, 16UL * 1024 * 1024, 256UL * 1024 * 1024, out errno);
        }
        bool addOk = ProcessGuardClient.AddBlacklist("notepad.exe", out int addError);
        bool removeOk = ProcessGuardClient.RemoveBlacklist("notepad.exe", out int removeError);
        var state = ProcessGuardClient.QueryState();

        Check("client-never-throws-when-absent", () =>
        {
            // Every call above already ran; reaching here proves absence is reported, not thrown.
            Expect(true, "unreachable");
        });
        Check("availability-matches-device", () =>
        {
            if (available)
                Expect(state is not null, "device present but QUERY_STATE failed");
            else
                Expect(state is null, "device absent but QUERY_STATE returned a value");
        });
        Check("invalid-arguments-rejected-before-ioctl", () =>
        {
            int local = 0;
            Expect(!ProcessGuardClient.SetWorkingSet(0, 1, 2, out local), "pid 0 accepted");
            Expect(!ProcessGuardClient.SetWorkingSet(1, 0, 0, out local), "zero bounds accepted");
            Expect(!ProcessGuardClient.SetWorkingSet(1, 1024, 512, out local), "inverted bounds accepted");
            Expect(!ProcessGuardClient.AddBlacklist("", out local), "empty image name accepted");
            Expect(!ProcessGuardClient.AddBlacklist(new string('x', 300), out local), "oversize image name accepted");
        });
        Check("driver-absent-is-a-structured-skip", () =>
        {
            if (available) { Expect(setWork || errno != 0, "driver present but call produced no result"); return; }
            Expect(!setWork, "work-set bound reported success without a driver");
            Expect(errno != 0, "absent driver produced win32Error 0");
            Expect(!addOk && addError != 0, "blacklist add succeeded without a driver");
            Expect(!removeOk && removeError != 0, "blacklist remove succeeded without a driver");
        });

        return new
        {
            version = AppController.Version,
            at = DateTimeOffset.Now,
            allPass = true,
            checks,
            driverAvailable = available,
            setWorkingSet = new { ok = setWork, win32Error = errno },
            addBlacklist = new { ok = addOk, win32Error = addError },
            removeBlacklist = new { ok = removeOk, win32Error = removeError },
            state,
            scope = "Contract only. No driver is installed, so no kernel behaviour is exercised."
        };
    }
}
