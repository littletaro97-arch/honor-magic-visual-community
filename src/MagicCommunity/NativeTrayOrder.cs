using System.IO;

namespace MagicCommunity;

internal static class NativeTrayOrder
{
    // Destination is an offered icon, never an arbitrary persisted index. The
    // renderer says which side of it matches its fixed-slot insertion preview.
    internal static List<string> Move(IReadOnlyList<string> order, string source, string destination, bool after)
    {
        if (order.Count > 256 || order.Any(string.IsNullOrEmpty) || order.Distinct(StringComparer.Ordinal).Count() != order.Count ||
            !order.Contains(source, StringComparer.Ordinal) || !order.Contains(destination, StringComparer.Ordinal))
            throw new InvalidDataException("Tray reorder target expired.");
        var next = order.ToList();
        if (source == destination) return next;
        next.Remove(source);
        next.Insert(next.IndexOf(destination) + (after ? 1 : 0), source);
        return next;
    }
}
