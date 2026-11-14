package com.armsx2.ui.trophies

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.widget.Toast
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.armsx2.ui.common.ArmsBackdrop
import com.armsx2.ui.common.ArmsTopBar
import com.armsx2.ui.common.RoundAction
import com.armsx3.core.ARMSX3
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Memory search for custom trophy rules (ARMSX3-Trophies).
 *
 * The classic "cheat search": look for a number you can see in the game, go back and change
 * it, then narrow the candidates down until only the address that really holds it is left.
 * Tapping a result copies a rule line for the trophy rules file (see CUSTOM_TROPHIES.md).
 *
 * The search itself lives in the core (_armsx3_memSearch) and survives closing this screen,
 * which is the whole workflow: search, resume the game, change the value, come back, filter.
 * The UI state is therefore kept in an object rather than in the composition.
 */
object MemorySearch {
    data class Hit(val address: String, val value: String)

    val type = mutableStateOf("u32")
    val value = mutableStateOf("")
    val busy = mutableStateOf(false)
    val message = mutableStateOf("")
    /** Candidate count, or -1 when no search is active. */
    val count = mutableStateOf(-1)
    val truncated = mutableStateOf(false)
    val hits = mutableStateOf<List<Hit>>(emptyList())
    /** Address the pointer list below belongs to ("" = none looked up yet). */
    val pointerFor = mutableStateOf("")
    /** Address expressions such as "[0xf3f20]+0x48", from the core's `pointer` command. */
    val pointers = mutableStateOf<List<String>>(emptyList())

    /** Looks for fixed places that lead to [address]. Blocking: call off the main thread. */
    fun findPointers(address: String) {
        val lines = call("pointer $address").lines()
        pointerFor.value = address
        if (lines.firstOrNull().orEmpty().startsWith("ok")) {
            pointers.value = lines.drop(1).map { it.trim() }.filter { it.isNotEmpty() }
            message.value = ""
        } else {
            pointers.value = emptyList()
            message.value = lines.firstOrNull().orEmpty().removePrefix("err").trim()
        }
    }

    /** The rules-file line that reads the value through pointer expression [expr]. */
    fun pointerRule(expr: String, fallbackValue: String): String {
        val prefix = if (type.value == "u32") "" else type.value
        val target = value.value.trim().ifEmpty { fallbackValue }
        return "when: \"$prefix[$expr] >= $target\""
    }

    /** Runs [command], then re-reads the first results. Blocking: call off the main thread. */
    fun execute(command: String?) {
        if (command != null) {
            if (command.startsWith("new") || command == "reset") {
                pointerFor.value = ""
                pointers.value = emptyList()
            }
            val reply = call(command)
            if (reply.startsWith("err")) {
                message.value = reply.removePrefix("err").trim()
                return
            }
        }

        val lines = call("list 60").lines()
        val head = lines.firstOrNull().orEmpty().split(' ')
        if (head.firstOrNull() != "ok" || head.size < 4) {
            // "err start a search first" is the normal state before the first search.
            count.value = -1
            hits.value = emptyList()
            val error = lines.firstOrNull().orEmpty().removePrefix("err").trim()
            message.value = if (command == null || command == "reset" || error == "start a search first") "" else error
            return
        }

        if (head[3] == "-") {
            count.value = -1
            hits.value = emptyList()
            message.value = ""
            return
        }

        count.value = head[1].toIntOrNull() ?: 0
        truncated.value = head[2] == "1"
        type.value = head[3]
        hits.value = lines.drop(1).mapNotNull { line ->
            val parts = line.trim().split(' ')
            if (parts.size >= 2) Hit(parts[0], parts[1]) else null
        }
        message.value = ""
    }

    private fun call(command: String): String =
        runCatching { ARMSX3.instance.memSearch(command) }.getOrNull()
            ?: "err this build of the emulator core has no memory search"

    /** The rules-file line for [hit], using the number last searched for as the target. */
    fun ruleFor(hit: Hit): String {
        val prefix = if (type.value == "u32") "" else type.value
        val target = value.value.trim().ifEmpty { hit.value }
        return "when: \"$prefix[0x${hit.address}] >= $target\""
    }
}

@Composable
fun MemorySearchScreen(onBack: () -> Unit) {
    val scope = rememberCoroutineScope()
    val context = LocalContext.current
    val busy = MemorySearch.busy.value
    val count = MemorySearch.count.value
    val active = count >= 0

    val run: (String?) -> Unit = { command -> launchSearch(scope, command) }
    val copy: (String) -> Unit = { rule ->
        val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager
        clipboard?.setPrimaryClip(ClipData.newPlainText("Trophy rule", rule))
        Toast.makeText(context, "Copied: $rule", Toast.LENGTH_SHORT).show()
    }

    // Pick up a search that is still alive in the core from an earlier visit.
    androidx.compose.runtime.LaunchedEffect(Unit) { run(null) }

    ArmsBackdrop {
        Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
            ArmsTopBar(
                title = "Memory search",
                subtitle = when {
                    busy -> "Searching…"
                    !active -> "Find the number behind a custom trophy"
                    MemorySearch.truncated.value -> "More than $count matches - narrow it down"
                    else -> "$count matches"
                },
                leading = { RoundAction("←", "Back", onBack) },
            )

            Column(
                Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                Text(
                    if (!active) {
                        "1. Type a number you can see in the game (coins, lives, score) and search. " +
                            "For a hidden count, tap \"I don't know the number\" instead.\n" +
                            "2. Go back to the game and change that number.\n" +
                            "3. Come back here and filter. Repeat until only a few are left."
                    } else {
                        "Go back to the game, change the number, then filter here. " +
                            "Tap a result to copy a rule line for the trophy rules file."
                    },
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )

                if (!active) {
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        TypeButton("u32", "Whole number")
                        TypeButton("f32", "Decimal")
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        TypeButton("u16", "Small (0-65535)")
                        TypeButton("u8", "Tiny (0-255)")
                    }
                }

                OutlinedTextField(
                    value = MemorySearch.value.value,
                    onValueChange = { MemorySearch.value.value = it },
                    label = { Text("Number in the game right now") },
                    singleLine = true,
                    enabled = !busy,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                    modifier = Modifier.fillMaxWidth(),
                )

                val typed = MemorySearch.value.value.trim()

                if (!active) {
                    Button(
                        onClick = { run("new ${MemorySearch.type.value} $typed") },
                        enabled = !busy && typed.isNotEmpty(),
                        modifier = Modifier.fillMaxWidth(),
                    ) { Text("Search") }

                    // For values the game never shows, like a hidden "blocks destroyed" count.
                    OutlinedButton(
                        onClick = { run("new ${MemorySearch.type.value} ?") },
                        enabled = !busy,
                        modifier = Modifier.fillMaxWidth(),
                    ) { Text("I don't know the number") }
                } else {
                    Button(
                        onClick = { run("filter eq $typed") },
                        enabled = !busy && typed.isNotEmpty(),
                        modifier = Modifier.fillMaxWidth(),
                    ) { Text("Filter: it is now this number") }

                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        FilterButton("Went up", "filter gt", busy, run)
                        FilterButton("Went down", "filter lt", busy, run)
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        FilterButton("Changed", "filter ne", busy, run)
                        FilterButton("Stayed the same", "filter same", busy, run)
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        OutlinedButton(onClick = { run(null) }, enabled = !busy) { Text("Refresh values") }
                        OutlinedButton(onClick = { run("reset") }, enabled = !busy) { Text("Start over") }
                    }
                }

                val message = MemorySearch.message.value
                if (message.isNotEmpty()) {
                    Text(
                        message,
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.error,
                    )
                }

                if (active) {
                    val hits = MemorySearch.hits.value
                    if (hits.isEmpty()) {
                        Text(
                            if (count > 0) {
                                "Too many to list yet. Do the thing in the game (break a block), then tap " +
                                    "\"Went up\". Do nothing for a moment, then tap \"Stayed the same\". " +
                                    "Keep alternating until the list appears."
                            } else {
                                "Nothing left. Start over, and try a different number type if it keeps happening."
                            },
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }

                    hits.forEach { hit ->
                        Surface(
                            shape = RoundedCornerShape(12.dp),
                            color = MaterialTheme.colorScheme.surfaceVariant,
                            modifier = Modifier.fillMaxWidth().clickable { copy(MemorySearch.ruleFor(hit)) },
                        ) {
                            Row(
                                Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp),
                                horizontalArrangement = Arrangement.SpaceBetween,
                                verticalAlignment = androidx.compose.ui.Alignment.CenterVertically,
                            ) {
                                Text(
                                    "0x${hit.address}",
                                    fontFamily = FontFamily.Monospace,
                                    color = MaterialTheme.colorScheme.onSurface,
                                )
                                Text(
                                    hit.value,
                                    fontFamily = FontFamily.Monospace,
                                    color = MaterialTheme.colorScheme.primary,
                                )
                                OutlinedButton(
                                    onClick = { launchWork(scope) { MemorySearch.findPointers(hit.address) } },
                                    enabled = !busy,
                                ) { Text("Find pointer") }
                            }
                        }

                        if (MemorySearch.pointerFor.value == hit.address) {
                            val pointers = MemorySearch.pointers.value
                            Text(
                                if (pointers.isEmpty()) {
                                    "No pointer found for 0x${hit.address}. A plain address rule may still work; " +
                                        "check it after restarting the game."
                                } else {
                                    "Pointer rules for 0x${hit.address} (likeliest first). Tap one to copy. " +
                                        "These should keep working after a restart; if one does not, try the next."
                                },
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                            pointers.forEach { expr ->
                                Surface(
                                    shape = RoundedCornerShape(12.dp),
                                    color = MaterialTheme.colorScheme.secondaryContainer,
                                    modifier = Modifier.fillMaxWidth()
                                        .clickable { copy(MemorySearch.pointerRule(expr, hit.value)) },
                                ) {
                                    Text(
                                        "[$expr]",
                                        fontFamily = FontFamily.Monospace,
                                        color = MaterialTheme.colorScheme.onSecondaryContainer,
                                        modifier = Modifier.padding(horizontal = 12.dp, vertical = 10.dp),
                                    )
                                }
                            }
                        }
                    }

                    if (count > hits.size) {
                        Text(
                            "Showing the first ${hits.size} of $count.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }

                    Text(
                        "A plain address is right for this play session, but some games move their " +
                            "data each time they start. \"Find pointer\" looks for a rule that follows " +
                            "the game to wherever the value is.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
    }
}

private fun launchSearch(scope: CoroutineScope, command: String?) =
    launchWork(scope) { MemorySearch.execute(command) }

private fun launchWork(scope: CoroutineScope, work: () -> Unit) {
    if (MemorySearch.busy.value) return
    MemorySearch.busy.value = true
    scope.launch {
        try {
            withContext(Dispatchers.IO) { work() }
        } finally {
            MemorySearch.busy.value = false
        }
    }
}

@Composable
private fun TypeButton(type: String, label: String) {
    if (MemorySearch.type.value == type) {
        Button(onClick = {}) { Text(label) }
    } else {
        OutlinedButton(onClick = { MemorySearch.type.value = type }) { Text(label) }
    }
}

@Composable
private fun FilterButton(label: String, command: String, busy: Boolean, run: (String?) -> Unit) {
    OutlinedButton(onClick = { run(command) }, enabled = !busy) { Text(label) }
}
