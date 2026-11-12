using MagicCommunity;
using System.Text.Json;

internal static class TrayOrderChecks
{
    internal static void Run(string nativeContract, string output)
    {
        using var document = JsonDocument.Parse(File.ReadAllText(nativeContract));
        int cases = 0, rejected = 0;
        foreach (var item in document.RootElement.EnumerateArray())
        {
            var original = item.GetProperty("original").EnumerateArray().Select(x => x.GetString()!).ToArray();
            var expected = item.GetProperty("preview").EnumerateArray().Select(x => x.GetString()!).ToArray();
            var actual = NativeTrayOrder.Move(original, item.GetProperty("id").GetString()!, item.GetProperty("destination").GetString()!, item.GetProperty("after").GetBoolean());
            if (!actual.SequenceEqual(expected)) throw new InvalidOperationException($"Native preview / saved order mismatch, case {cases}.");
            ++cases;
        }
        foreach (var (order, source, destination) in new (string[], string, string)[]
        {
            (["a", "b"], "missing", "b"),
            (["a", "b"], "a", "missing"),
            (["a", "a", "b"], "a", "b"),
            (["a", "", "b"], "a", "b"),
            ([], "a", "b"),
            (Enumerable.Range(0,257).Select(x => x.ToString()).ToArray(), "0", "1")
        })
        {
            try { NativeTrayOrder.Move(order, source, destination, true); }
            catch (InvalidDataException) { ++rejected; continue; }
            throw new InvalidOperationException("Invalid or expired tray target was accepted.");
        }
        // Order persistence uses stable IDs; source inventory and pin state are
        // owned separately and must not be changed while constructing the plan.
        string[] inventory = ["a", "b", "c"];
        var reordered = NativeTrayOrder.Move(inventory, "a", "c", true);
        if (!inventory.SequenceEqual(new[] { "a", "b", "c" }) ||
            !NativeTrayOrder.Move(reordered, "a", "b", false).SequenceEqual(inventory))
            throw new InvalidOperationException("Round trip mutated the input or failed to restore order.");
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(output))!);
        File.WriteAllText(output, JsonSerializer.Serialize(new
        {
            allPass = true, nativePreviewCases = cases, invalidTargetsRejected = rejected, roundTrip = true,
            scope = "Actual native insertion requests compared with the production managed ordering helper; synthetic IDs only, no user preference writes."
        }, new JsonSerializerOptions { WriteIndented = true }));
    }
}
