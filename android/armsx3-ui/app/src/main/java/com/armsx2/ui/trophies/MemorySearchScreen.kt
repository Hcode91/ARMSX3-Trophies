package com.armsx2.ui.trophies

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
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
import kotlinx.coroutines.delay
import com.armsx2.ui.settings.controllerFocusable
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.offset
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.runtime.LaunchedEffect
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

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
        return "$prefix[$expr] >= $target"
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

    /** The rule for [hit], using the number last searched for as the target. */
    fun ruleFor(hit: Hit): String {
        val prefix = if (type.value == "u32") "" else type.value
        val target = value.value.trim().ifEmpty { hit.value }
        return "$prefix[0x${hit.address}] >= $target"
    }

    // ---- Watch list: expressions whose live value is shown, here and over the game. ----
    // Kept per trophy set in trophy_rules/<set>_watch.txt, so it survives a restart: that is
    // how a pointer is checked (add the candidates, restart the game, see which still shows
    // the right number).
    val watches = mutableStateOf<List<String>>(emptyList())
    val watchValues = mutableStateOf<Map<String, String>>(emptyMap())
    /** Show the watch list (and the filter buttons) over the running game. */
    val overlay = mutableStateOf(false)
    /** Where the overlay was dragged to, in pixels from the top-left corner. */
    val overlayX = mutableStateOf(0f)
    val overlayY = mutableStateOf(0f)
    private var watchSet = ""

    private fun watchFile(): File? {
        val set = runCatching { ARMSX3.instance.getCurrentTrophyName() }.getOrNull()?.trim().orEmpty()
        if (set.isEmpty()) return null
        val user = runCatching { ARMSX3.instance.getUser() }.getOrNull()?.takeIf { it.isNotBlank() } ?: "00000001"
        return File(File(ARMSX3.getHdd0Dir(), "home/$user/trophy_rules"), "${set}_watch.txt")
    }

    /** Loads the running game's saved watch list (once per game). Blocking. */
    fun loadWatches() {
        val file = watchFile() ?: return
        if (file.path == watchSet) return
        watchSet = file.path
        watches.value = runCatching {
            if (file.isFile) file.readLines().map { it.trim() }.filter { it.isNotEmpty() }.distinct().take(12) else emptyList()
        }.getOrDefault(emptyList())
    }

    private fun saveWatches() {
        val file = watchFile() ?: return
        runCatching {
            file.parentFile?.mkdirs()
            file.writeText(watches.value.joinToString("\n"))
        }
    }

    /** Blocking. */
    fun addWatch(expr: String) {
        val clean = expr.trim()
        if (clean.isEmpty() || clean in watches.value) return
        watches.value = (watches.value + clean).takeLast(12)
        saveWatches()
        pollWatches()
    }

    /** Blocking. */
    fun removeWatch(expr: String) {
        watches.value = watches.value - expr
        saveWatches()
    }

    /** Re-reads every watched value. Blocking. */
    fun pollWatches() {
        val list = watches.value
        if (list.isEmpty()) {
            if (watchValues.value.isNotEmpty()) watchValues.value = emptyMap()
            return
        }
        watchValues.value = list.associateWith { expr ->
            val reply = call("eval $expr")
            if (reply.startsWith("ok")) reply.removePrefix("ok").trim() else "?"
        }
    }

    /** Candidate count only, for the overlay. Blocking. */
    fun pollCount() {
        val head = call("list 0").lines().firstOrNull().orEmpty().split(' ')
        count.value = if (head.firstOrNull() == "ok" && head.size >= 4 && head[3] != "-") head[1].toIntOrNull() ?: 0 else -1
    }

    /** The value part of a rule: "[0x10] >= 5" -> "[0x10]". */
    fun exprOf(rule: String): String {
        var cut = rule.length
        for (op in listOf(">=", "<=", "==", "!=", ">", "<")) {
            val at = rule.lastIndexOf(op)
            if (at in 1 until cut) cut = at
        }
        return rule.substring(0, cut).trim()
    }

    // ---- "Create trophy" form. Non-null draftRule = the form is open. ----
    val draftRule = mutableStateOf<String?>(null)
    val draftName = mutableStateOf("")
    val draftDetail = mutableStateOf("")
    val draftGrade = mutableStateOf("B")
    val draftIcon = mutableStateOf<Uri?>(null)
    /** Shown after a trophy was written. */
    val created = mutableStateOf("")

    fun openDraft(rule: String) {
        draftRule.value = rule
        draftName.value = ""
        draftDetail.value = ""
        draftGrade.value = "B"
        draftIcon.value = null
        created.value = ""
        message.value = ""
        editing.value = null
    }

    private fun quote(text: String): String =
        "\"" + text.replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", " ") + "\""

    // ---- The running game's pack: trophy_rules/<set>.yml plus its icon folder. ----

    /** One entry of the file's `custom:` list, kept as its raw lines so anything this screen
     *  does not understand (steps, reset, comments) survives an edit untouched. */
    class CustomEntry(val lines: MutableList<String>) {
        private fun isField(line: String) = line.length - line.trimStart().length <= 4
        private fun keyOf(line: String) = line.trim().removePrefix("-").trim().substringBefore(':').trim()

        fun get(key: String): String? {
            val line = lines.firstOrNull { isField(it) && keyOf(it) == key } ?: return null
            val raw = line.substringAfter(':').trim()
            return if (raw.length >= 2 && raw.startsWith("\"") && raw.endsWith("\"")) {
                raw.substring(1, raw.length - 1).replace("\\\"", "\"").replace("\\\\", "\\")
            } else {
                raw.substringBefore(" #").trim()
            }
        }

        fun set(key: String, value: String) {
            val at = lines.indexOfFirst { isField(it) && keyOf(it) == key }
            val line = (if (at == 0) "  - " else "    ") + "$key: $value"
            if (at >= 0) lines[at] = line else lines.add(line)
        }

        val deleted: Boolean get() = get("deleted") == "true"
        val name: String get() = get("name").orEmpty()
    }

    private class Pack(val file: File, val header: MutableList<String>, val entries: MutableList<CustomEntry>)

    /** (index in the file, entry) of every custom trophy that is not deleted. */
    val customList = mutableStateOf<List<Pair<Int, CustomEntry>>>(emptyList())
    /** Index of the entry the form is editing, or null when it is creating a new one. */
    val editing = mutableStateOf<Int?>(null)

    private fun rulesDir(): File {
        val user = runCatching { ARMSX3.instance.getUser() }.getOrNull()?.takeIf { it.isNotBlank() } ?: "00000001"
        return File(ARMSX3.getHdd0Dir(), "home/$user/trophy_rules")
    }

    private fun currentSet(): String =
        runCatching { ARMSX3.instance.getCurrentTrophyName() }.getOrNull()?.trim().orEmpty()

    /** Reads the running game's pack. Throws with a readable message if the file has a shape
     *  this screen cannot edit safely. */
    private fun readPack(): Pack? {
        val set = currentSet()
        if (set.isEmpty()) return null
        val file = File(rulesDir(), "$set.yml")

        if (!file.isFile) {
            val info = call("info")
            val hash = info.removePrefix("ok").trim().takeIf { info.startsWith("ok") && it.startsWith("PPU-") }
            val header = mutableListOf("# Custom trophies for $set")
            if (hash != null) header += "game: $hash"
            header += "custom:"
            return Pack(file, header, mutableListOf())
        }

        val lines = file.readLines()
        val at = lines.indexOfFirst { it.startsWith("custom:") }
        if (at < 0) {
            val header = lines.toMutableList()
            while (header.isNotEmpty() && header.last().isBlank()) header.removeAt(header.size - 1)
            header += "custom:"
            return Pack(file, header, mutableListOf())
        }
        if (lines[at].removePrefix("custom:").substringBefore('#').isNotBlank()) {
            throw IllegalStateException("In ${file.name}, custom: has to be a list with one trophy per \"- name:\" entry.")
        }

        val entries = mutableListOf<CustomEntry>()
        for (line in lines.drop(at + 1)) {
            if (line.isBlank()) continue
            val indented = line.startsWith(" ") || line.startsWith("\t")
            if (!indented && !line.startsWith("-") && !line.startsWith("#")) {
                throw IllegalStateException("In ${file.name}, move the custom: section to the end of the file first.")
            }
            if (line.trimStart().startsWith("- ") && line.length - line.trimStart().length <= 2) {
                entries += CustomEntry(mutableListOf(line))
            } else {
                entries.lastOrNull()?.lines?.add(line)
            }
        }
        return Pack(file, lines.take(at + 1).toMutableList(), entries)
    }

    private fun writePack(pack: Pack) {
        pack.file.parentFile?.mkdirs()
        pack.file.writeText((pack.header + pack.entries.flatMap { it.lines }).joinToString("\n") + "\n")
    }

    /** What the core could not understand in this game's rules file the last time it loaded
     *  it (trophy_rules/<set>_problems.txt), one problem per line. */
    val problems = mutableStateOf<List<String>>(emptyList())

    /** Locks a custom trophy again and clears its rule's progress, so its rule can be tested
     *  again. Blocking. Returns an error message, or null. */
    fun resetCustom(index: Int): String? {
        val reply = call("resetcustom $index")
        return if (reply.startsWith("ok")) null else reply.removePrefix("err").trim()
    }

    /** Re-reads the list of this game's custom trophies. Blocking. */
    fun refreshCustom() {
        problems.value = runCatching {
            val set = currentSet()
            if (set.isEmpty()) emptyList()
            else File(rulesDir(), "${set}_problems.txt").takeIf { it.isFile }?.readLines().orEmpty().filter { it.isNotBlank() }
        }.getOrDefault(emptyList())
        customList.value = runCatching {
            readPack()?.entries.orEmpty().withIndex().filter { !it.value.deleted }.map { it.index to it.value }
        }.getOrDefault(emptyList())
    }

    /** Opens the form on an existing custom trophy. */
    fun openEdit(index: Int, entry: CustomEntry) {
        openDraft(entry.get("when").orEmpty())
        editing.value = index
        draftName.value = entry.name
        draftDetail.value = entry.get("detail").orEmpty()
        draftGrade.value = entry.get("grade")?.take(1)?.uppercase()?.takeIf { it in listOf("B", "S", "G", "P") } ?: "B"
    }

    /**
     * Deletes a custom trophy. It keeps its place in the file, marked `deleted: true`: a custom
     * trophy's number is its position, so truly removing it would move every later trophy's
     * earned state onto the wrong trophy. The core hides it and never awards it.
     * Blocking. Returns an error message, or null.
     */
    fun deleteCustom(index: Int): String? = runCatching<String?> {
        val pack = readPack() ?: return@runCatching "No game is running."
        val entry = pack.entries.getOrNull(index) ?: return@runCatching "That trophy is no longer in the file."
        entry.set("deleted", "true")
        writePack(pack)
        refreshCustom()
        null
    }.getOrElse { it.message ?: "Could not delete the trophy." }

    /**
     * Saves the form: a new entry at the end of the running game's `custom:` list, or the edited
     * fields of an existing one. The core merges the list into the trophy list the next time
     * the game starts (see CUSTOM_TROPHIES.md).
     * Blocking: call off the main thread. Returns an error message, or null on success.
     */
    /** The rule box takes a bare condition, but people paste whole `when: "..."` lines from
     *  a rules file (or a chat) into it, sometimes on top of what was already there. Keep only
     *  the condition itself. */
    fun cleanRule(text: String): String {
        var rule = text.trim()
        val at = rule.lastIndexOf("when:")
        if (at >= 0) rule = rule.substring(at + 5).trim()
        rule = rule.replace("\\\"", "\"")
        while (rule.length >= 2 && rule.startsWith("\"") && rule.endsWith("\"")) {
            rule = rule.substring(1, rule.length - 1).trim()
        }
        return rule.trim('"').trim()
    }

    fun createTrophy(context: Context): String? {
        val rule = cleanRule(draftRule.value.orEmpty())
        val name = draftName.value.trim()
        val editIndex = editing.value
        if (name.isEmpty()) return "Give the trophy a name."
        val platinum = draftGrade.value == "P"
        if (rule.isEmpty() && editIndex == null && !platinum) return "The rule is empty."

        val set = currentSet()
        if (set.isEmpty()) return "This game has not set up its trophies yet. Play until it has, then try again."

        return runCatching<String?> {
            val pack = readPack() ?: return@runCatching "No game is running."

            if (platinum && pack.entries.withIndex().any { it.index != editIndex && !it.value.deleted && it.value.get("grade")?.startsWith("P", ignoreCase = true) == true }) {
                return@runCatching "This game already has a custom platinum."
            }

            var iconName: String? = null
            draftIcon.value?.let { uri ->
                val bitmap = context.contentResolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it) }
                if (bitmap != null) {
                    val iconDir = File(rulesDir(), set)
                    iconDir.mkdirs()
                    iconName = "trophy_${System.currentTimeMillis()}.png"
                    val scaled = Bitmap.createScaledBitmap(bitmap, 240, 240, true)
                    File(iconDir, iconName!!).outputStream().use { scaled.compress(Bitmap.CompressFormat.PNG, 100, it) }
                }
            }

            val entry = if (editIndex != null) {
                pack.entries.getOrNull(editIndex) ?: return@runCatching "That trophy is no longer in the file."
            } else {
                CustomEntry(mutableListOf("  - name: ${quote(name)}")).also { pack.entries += it }
            }

            entry.set("name", quote(name))
            entry.set("detail", quote(draftDetail.value.trim()))
            entry.set("grade", draftGrade.value)
            iconName?.let { entry.set("icon", it) }
            // An empty rule box on an edit leaves the existing rule (which may be a sequence
            // this form cannot show) exactly as it is.
            // A platinum has no rule of its own: it is earned by unlocking everything else.
            if (rule.isNotEmpty() && !platinum) entry.set("when", quote(rule))

            writePack(pack)
            editing.value = null
            refreshCustom()
            null
        }.getOrElse { it.message ?: "Could not save the trophy." }
    }

    /** Writes the running game's pack (rules file + icons) as a zip to [target]. Blocking. */
    fun exportPack(context: Context, target: Uri, everyGame: Boolean = false): String = runCatching {
        val setName = Regex("^NPWR\\d{5}_\\d{2}$")
        val sets = if (everyGame) {
            rulesDir().listFiles().orEmpty()
                .filter { it.isFile && it.name.endsWith(".yml") && setName.matches(it.name.removeSuffix(".yml")) }
                .map { it.name.removeSuffix(".yml") }.sorted()
        } else {
            val set = currentSet()
            if (set.isEmpty()) return@runCatching "No game is running."
            listOf(set).filter { File(rulesDir(), "$it.yml").isFile }
        }
        if (sets.isEmpty()) return@runCatching if (everyGame) "There are no custom trophy packs yet." else "This game has no custom trophies yet."
        val out = context.contentResolver.openOutputStream(target) ?: return@runCatching "Could not write to that location."
        java.util.zip.ZipOutputStream(out).use { zip ->
            for (set in sets) {
                zip.putNextEntry(java.util.zip.ZipEntry("$set.yml"))
                zip.write(File(rulesDir(), "$set.yml").readBytes())
                zip.closeEntry()
                File(rulesDir(), set).listFiles().orEmpty()
                    .filter { it.isFile && it.name.endsWith(".png", ignoreCase = true) }
                    .forEach { icon ->
                        zip.putNextEntry(java.util.zip.ZipEntry("$set/${icon.name}"))
                        zip.write(icon.readBytes())
                        zip.closeEntry()
                    }
            }
        }
        if (sets.size == 1) "Pack exported." else "${sets.size} packs exported."
    }.getOrElse { "Could not export the pack: ${it.message}" }

    /**
     * Installs a pack zip made by [exportPack]. Only a `<NPWRxxxxx_00>.yml` and PNGs in the
     * matching folder are taken, so a zip cannot write anywhere else. An existing rules file
     * for that game is kept as `.bak`. Blocking.
     */
    fun importPack(context: Context, source: Uri): String = runCatching {
        val ymlName = Regex("^(NPWR\\d{5}_\\d{2})\\.yml$")
        val iconName = Regex("^(NPWR\\d{5}_\\d{2})/([A-Za-z0-9_.-]+\\.(?:png|PNG))$")
        val limit = 5 * 1024 * 1024
        val dir = rulesDir()
        var installed: String? = null
        var icons = 0

        val input = context.contentResolver.openInputStream(source) ?: return@runCatching "Could not read that file."
        java.util.zip.ZipInputStream(input).use { zip ->
            while (true) {
                val entry = zip.nextEntry ?: break
                val name = entry.name.replace('\\', '/')
                val yml = ymlName.find(name)
                val icon = iconName.find(name)
                if (entry.isDirectory || (yml == null && icon == null)) continue

                val bytes = java.io.ByteArrayOutputStream()
                val buffer = ByteArray(65536)
                while (true) {
                    val read = zip.read(buffer)
                    if (read < 0) break
                    bytes.write(buffer, 0, read)
                    if (bytes.size() > limit) return@runCatching "That pack has a file that is too large."
                }

                if (yml != null) {
                    dir.mkdirs()
                    val target = File(dir, "${yml.groupValues[1]}.yml")
                    if (target.isFile) target.copyTo(File(dir, "${target.name}.bak"), overwrite = true)
                    target.writeBytes(bytes.toByteArray())
                    installed = yml.groupValues[1]
                } else if (icon != null && ".." !in icon.groupValues[2]) {
                    val iconDir = File(dir, icon.groupValues[1])
                    iconDir.mkdirs()
                    File(iconDir, icon.groupValues[2]).writeBytes(bytes.toByteArray())
                    icons++
                }
            }
        }

        if (installed == null) "That zip is not a trophy pack."
        else "Imported the pack for $installed ($icons icons). Start that game (again) to see its trophies."
    }.getOrElse { "Could not import the pack: ${it.message}" }
}

@Composable
fun MemorySearchScreen(onBack: () -> Unit) {
    val scope = rememberCoroutineScope()
    val context = LocalContext.current
    val busy = MemorySearch.busy.value
    val count = MemorySearch.count.value
    val active = count >= 0

    val run: (String?) -> Unit = { command -> launchSearch(scope, command) }
    // Tapping a result opens the "create trophy" form with its rule filled in.
    val copy: (String) -> Unit = { rule -> MemorySearch.openDraft(rule) }
    val copyText: (String) -> Unit = { rule ->
        val line = "when: \"$rule\""
        val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager
        clipboard?.setPrimaryClip(ClipData.newPlainText("Trophy rule", line))
        Toast.makeText(context, "Copied: $line", Toast.LENGTH_SHORT).show()
    }
    val pickIcon = androidx.activity.compose.rememberLauncherForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.GetContent(),
    ) { uri -> if (uri != null) MemorySearch.draftIcon.value = uri }
    val exportPack = androidx.activity.compose.rememberLauncherForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.CreateDocument("application/zip"),
    ) { uri -> if (uri != null) launchWork(scope) { MemorySearch.created.value = MemorySearch.exportPack(context, uri) } }
    val importPack = androidx.activity.compose.rememberLauncherForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.GetContent(),
    ) { uri -> if (uri != null) launchWork(scope) { MemorySearch.created.value = MemorySearch.importPack(context, uri); MemorySearch.refreshCustom() } }

    // Pick up a search that is still alive in the core from an earlier visit.
    androidx.compose.runtime.LaunchedEffect(Unit) { run(null) }
    LaunchedEffect(Unit) {
        withContext(Dispatchers.IO) {
            MemorySearch.loadWatches()
            MemorySearch.pollWatches()
            MemorySearch.refreshCustom()
        }
    }

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

            val draft = MemorySearch.draftRule.value
            if (draft != null) {
                Column(
                    Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    Text(if (MemorySearch.editing.value != null) "Edit trophy" else "New trophy", style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.onSurface)
                    OutlinedTextField(
                        value = MemorySearch.draftName.value,
                        onValueChange = { MemorySearch.draftName.value = it },
                        label = { Text("Name") },
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth(),
                    )
                    OutlinedTextField(
                        value = MemorySearch.draftDetail.value,
                        onValueChange = { MemorySearch.draftDetail.value = it },
                        label = { Text("Description") },
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        GradeButton("B", "Bronze")
                        GradeButton("S", "Silver")
                        GradeButton("G", "Gold")
                    }
                    GradeButton("P", "Platinum - earned by unlocking every other trophy")
                    if (MemorySearch.draftGrade.value != "P") {
                        OutlinedTextField(
                            value = draft,
                            onValueChange = { MemorySearch.draftRule.value = it },
                            label = { Text("Earned when (change the number at the end to your target)") },
                            singleLine = true,
                            modifier = Modifier.fillMaxWidth(),
                        )
                        // Shortcuts that rewrite the rule around the value it watches
                        val watched = MemorySearch.exprOf(MemorySearch.cleanRule(draft))
                        if (watched.isNotEmpty() && !watched.startsWith("delta(") && !watched.startsWith("count(")) {
                            Text(
                                "Make it about…",
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                OutlinedButton(onClick = { MemorySearch.draftRule.value = "delta($watched) >= 10" }) { Text("This session only") }
                                OutlinedButton(onClick = { MemorySearch.draftRule.value = "$watched < prev($watched)" }) { Text("It going down") }
                            }
                            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                OutlinedButton(onClick = { MemorySearch.draftRule.value = "$watched > prev($watched)" }) { Text("It going up") }
                                OutlinedButton(onClick = { MemorySearch.draftRule.value = "count($watched > 0) >= 10" }) { Text("How many times") }
                            }
                        }
                        val plain = MemorySearch.cleanRule(draft)
                        if (plain.isNotEmpty() && " for " !in plain && "||" !in plain && "&&" !in plain) {
                            OutlinedButton(onClick = { MemorySearch.draftRule.value = "$plain for 30s" }) { Text("Has to stay true for 30 seconds") }
                        }
                    }
                    OutlinedButton(onClick = { pickIcon.launch("image/*") }, modifier = Modifier.fillMaxWidth()) {
                        Text(if (MemorySearch.draftIcon.value == null) "Pick an icon (optional)" else "Icon picked - tap to change")
                    }
                    Button(
                        onClick = {
                            launchWork(scope) {
                                val error = MemorySearch.createTrophy(context)
                                if (error == null) {
                                    MemorySearch.created.value =
                                        "\"${MemorySearch.draftName.value.trim()}\" saved. Close the game and start it again to see the change in the trophy list."
                                    MemorySearch.draftRule.value = null
                                    MemorySearch.message.value = ""
                                } else {
                                    MemorySearch.message.value = error
                                }
                            }
                        },
                        enabled = !busy && MemorySearch.draftName.value.isNotBlank(),
                        modifier = Modifier.fillMaxWidth(),
                    ) { Text(if (MemorySearch.editing.value != null) "Save changes" else "Create trophy") }
                    OutlinedButton(
                        onClick = {
                            launchWork(scope) { MemorySearch.addWatch(MemorySearch.exprOf(draft)) }
                            MemorySearch.draftRule.value = null
                        },
                        modifier = Modifier.fillMaxWidth(),
                    ) { Text("Just watch this value for now") }
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        OutlinedButton(onClick = { copyText(draft) }) { Text("Copy rule text") }
                        OutlinedButton(onClick = { MemorySearch.draftRule.value = null; MemorySearch.message.value = "" }) { Text("Cancel") }
                    }
                    val error = MemorySearch.message.value
                    if (error.isNotEmpty()) {
                        Text(error, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.error)
                    }
                }
            } else
            Column(
                Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                val problems = MemorySearch.problems.value
                if (problems.isNotEmpty()) {
                    Surface(
                        shape = RoundedCornerShape(12.dp),
                        color = MaterialTheme.colorScheme.errorContainer,
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Text(
                                "Problems in this game's trophy rules (as of the last time the game started):",
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onErrorContainer,
                            )
                            problems.take(8).forEach {
                                Text("• $it", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onErrorContainer)
                            }
                        }
                    }
                }

                val created = MemorySearch.created.value
                if (created.isNotEmpty()) {
                    Text(created, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.primary)
                }

                Text(
                    if (!active) {
                        "1. Type a number you can see in the game (coins, lives, score) and search. " +
                            "For a hidden count, tap \"I don't know the number\" instead.\n" +
                            "2. Go back to the game and change that number.\n" +
                            "3. Come back here and filter. Repeat until only a few are left."
                    } else {
                        "Go back to the game, change the number, then filter here. " +
                            "Tap a result to make a trophy from it."
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
                        modifier = Modifier.fillMaxWidth().controllerFocusable("memsearch.search", onConfirm = {
                            if (!busy && typed.isNotEmpty()) run("new ${MemorySearch.type.value} $typed")
                        }),
                    ) { Text("Search") }

                    // For values the game never shows, like a hidden "blocks destroyed" count.
                    OutlinedButton(
                        onClick = { run("new ${MemorySearch.type.value} ?") },
                        enabled = !busy,
                        modifier = Modifier.fillMaxWidth().controllerFocusable("memsearch.unknown", onConfirm = {
                            if (!busy) run("new ${MemorySearch.type.value} ?")
                        }),
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
                    if (typed.isNotEmpty()) {
                        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                            FilterButton("Up by exactly $typed", "filter inc $typed", busy, run)
                            FilterButton("Down by exactly $typed", "filter dec $typed", busy, run)
                        }
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        OutlinedButton(onClick = { run(null) }, enabled = !busy) { Text("Refresh values") }
                        OutlinedButton(onClick = { run("reset") }, enabled = !busy) { Text("Start over") }
                    }
                }

                // This game's custom trophies: edit, delete, export, import
                val custom = MemorySearch.customList.value
                if (custom.isNotEmpty()) {
                    Text(
                        "This game's custom trophies (changes show after restarting the game):",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    custom.forEach { (index, entry) ->
                        Surface(
                            shape = RoundedCornerShape(12.dp),
                            color = MaterialTheme.colorScheme.surfaceVariant,
                            modifier = Modifier.fillMaxWidth(),
                        ) {
                            Row(
                                Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp),
                                horizontalArrangement = Arrangement.spacedBy(6.dp),
                                verticalAlignment = androidx.compose.ui.Alignment.CenterVertically,
                            ) {
                                Text(entry.name, color = MaterialTheme.colorScheme.onSurface, modifier = Modifier.weight(1f))
                                OutlinedButton(
                                    onClick = {
                                        launchWork(scope) {
                                            val error = MemorySearch.resetCustom(index)
                                            MemorySearch.created.value = if (error == null) "\"${entry.name}\" is locked again." else ""
                                            MemorySearch.message.value = error.orEmpty()
                                        }
                                    },
                                    enabled = !busy,
                                ) { Text("Reset") }
                                OutlinedButton(onClick = { MemorySearch.openEdit(index, entry) }) { Text("Edit") }
                                OutlinedButton(
                                    onClick = {
                                        launchWork(scope) {
                                            MemorySearch.deleteCustom(index)?.let { MemorySearch.message.value = it }
                                        }
                                    },
                                    enabled = !busy,
                                ) { Text("Delete") }
                            }
                        }
                    }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    OutlinedButton(
                        onClick = { exportPack.launch("custom-trophies.zip") },
                        enabled = !busy && custom.isNotEmpty(),
                    ) { Text("Export pack") }
                    OutlinedButton(onClick = { importPack.launch("*/*") }, enabled = !busy) { Text("Import pack") }
                }

                // Watch list + the over-the-game overlay
                val watches = MemorySearch.watches.value
                OutlinedButton(
                    onClick = { MemorySearch.overlay.value = !MemorySearch.overlay.value },
                    modifier = Modifier.fillMaxWidth().controllerFocusable("memsearch.overlay", onConfirm = {
                        MemorySearch.overlay.value = !MemorySearch.overlay.value
                    }),
                ) {
                    Text(
                        if (MemorySearch.overlay.value) "Showing values and filter buttons over the game - tap to hide"
                        else "Show values and filter buttons over the game",
                    )
                }
                if (watches.isNotEmpty()) {
                    Text(
                        "Watching (kept after a restart, so a pointer that still shows the right number is a good one):",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    val values = MemorySearch.watchValues.value
                    watches.forEach { expr ->
                        Surface(
                            shape = RoundedCornerShape(12.dp),
                            color = MaterialTheme.colorScheme.surfaceVariant,
                            modifier = Modifier.fillMaxWidth().clickable { MemorySearch.openDraft("$expr >= ${values[expr] ?: "1"}") },
                        ) {
                            Row(
                                Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp),
                                horizontalArrangement = Arrangement.SpaceBetween,
                                verticalAlignment = androidx.compose.ui.Alignment.CenterVertically,
                            ) {
                                Text(expr, fontFamily = FontFamily.Monospace, color = MaterialTheme.colorScheme.onSurface, modifier = Modifier.weight(1f))
                                Text(values[expr] ?: "?", fontFamily = FontFamily.Monospace, color = MaterialTheme.colorScheme.primary)
                                OutlinedButton(onClick = { launchWork(scope) { MemorySearch.removeWatch(expr) } }) { Text("Remove") }
                            }
                        }
                    }
                    OutlinedButton(onClick = { launchWork(scope) { MemorySearch.pollWatches() } }, enabled = !busy) { Text("Refresh watched values") }
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
                                    "Pointer rules for 0x${hit.address} (likeliest first). Tap one to make a trophy from it. " +
                                        "These should keep working after a restart; if one does not, try the next."
                                },
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                            if (pointers.isNotEmpty()) {
                                OutlinedButton(
                                    onClick = { launchWork(scope) { pointers.take(8).forEach { MemorySearch.addWatch("[$it]") } } },
                                    enabled = !busy,
                                ) { Text("Watch the first ${minOf(8, pointers.size)} to check them after a restart") }
                            }
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
        Button(onClick = {}, modifier = Modifier.controllerFocusable("memsearch.type.$type")) { Text(label) }
    } else {
        OutlinedButton(
            onClick = { MemorySearch.type.value = type },
            modifier = Modifier.controllerFocusable("memsearch.type.$type", onConfirm = { MemorySearch.type.value = type }),
        ) { Text(label) }
    }
}

@Composable
private fun GradeButton(grade: String, label: String) {
    if (MemorySearch.draftGrade.value == grade) {
        Button(onClick = {}) { Text(label) }
    } else {
        OutlinedButton(onClick = { MemorySearch.draftGrade.value = grade }) { Text(label) }
    }
}

@Composable
private fun FilterButton(label: String, command: String, busy: Boolean, run: (String?) -> Unit) {
    OutlinedButton(
        onClick = { run(command) },
        enabled = !busy,
        modifier = Modifier.controllerFocusable("memsearch.$command", onConfirm = { if (!busy) run(command) }),
    ) { Text(label) }
}

/**
 * The watch list and the filter buttons, drawn over the RUNNING game so a search does not need
 * a trip to the menu for every round. Composed from WindowImpl next to the touch controls;
 * draws nothing unless it was switched on from the memory search screen.
 */
@Composable
fun MemoryWatchOverlay() {
    if (!MemorySearch.overlay.value) return
    // Not while a menu or a full screen covers the game.
    if (com.armsx2.ui.WindowImpl.frontendCovers) return

    val scope = rememberCoroutineScope()

    LaunchedEffect(Unit) {
        while (true) {
            withContext(Dispatchers.IO) {
                MemorySearch.loadWatches()
                MemorySearch.pollWatches()
                if (!MemorySearch.busy.value) MemorySearch.pollCount()
            }
            delay(500)
        }
    }

    val watches = MemorySearch.watches.value
    val values = MemorySearch.watchValues.value
    val count = MemorySearch.count.value
    val busy = MemorySearch.busy.value

    Column(
        Modifier
            .offset { androidx.compose.ui.unit.IntOffset(MemorySearch.overlayX.value.toInt(), MemorySearch.overlayY.value.toInt()) }
            .padding(start = 8.dp, top = 8.dp)
            // Drag the panel anywhere on the screen
            .pointerInput(Unit) {
                detectDragGestures { change, drag ->
                    change.consume()
                    MemorySearch.overlayX.value = (MemorySearch.overlayX.value + drag.x).coerceAtLeast(0f)
                    MemorySearch.overlayY.value = (MemorySearch.overlayY.value + drag.y).coerceAtLeast(0f)
                }
            }
            .background(androidx.compose.ui.graphics.Color.Black.copy(alpha = 0.6f), RoundedCornerShape(10.dp))
            .padding(horizontal = 8.dp, vertical = 6.dp),
        verticalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        watches.take(6).forEach { expr ->
            Text(
                "${values[expr] ?: "?"}   $expr",
                fontFamily = FontFamily.Monospace,
                color = androidx.compose.ui.graphics.Color.White,
                style = MaterialTheme.typography.bodySmall,
            )
        }
        if (count >= 0) {
            Text(
                if (busy) "Filtering…" else "$count matches",
                color = androidx.compose.ui.graphics.Color.White,
                style = MaterialTheme.typography.bodySmall,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                OverlayChip("Up") { launchWork(scope) { MemorySearch.execute("filter gt") } }
                OverlayChip("Down") { launchWork(scope) { MemorySearch.execute("filter lt") } }
                OverlayChip("Same") { launchWork(scope) { MemorySearch.execute("filter same") } }
                OverlayChip("Changed") { launchWork(scope) { MemorySearch.execute("filter ne") } }
            }
        }
        if (watches.isEmpty() && count < 0) {
            Text(
                "Nothing to watch yet",
                color = androidx.compose.ui.graphics.Color.White,
                style = MaterialTheme.typography.bodySmall,
            )
        }
        OverlayChip("Hide") { MemorySearch.overlay.value = false }
    }
}

@Composable
private fun OverlayChip(label: String, onClick: () -> Unit) {
    Text(
        label,
        color = androidx.compose.ui.graphics.Color.White,
        style = MaterialTheme.typography.bodySmall,
        modifier = Modifier
            .background(androidx.compose.ui.graphics.Color.White.copy(alpha = 0.18f), RoundedCornerShape(8.dp))
            .clickable(onClick = onClick)
            .padding(horizontal = 10.dp, vertical = 6.dp),
    )
}
