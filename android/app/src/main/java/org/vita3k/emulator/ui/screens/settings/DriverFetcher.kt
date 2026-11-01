package org.vita3k.emulator.ui.screens.settings

import android.content.Context
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

// In-app GPU driver fetcher (same repos as the Eden driver fetcher).
// Lists the .zip assets of each repo's GitHub releases, downloads the chosen one and hands the file to onInstall.
private val DRIVER_REPOS = listOf(
    "MrPurple666/purple-turnip",
    "crueter/GameHub-8Elite-Drivers",
    "K11MCH1/AdrenoToolsDrivers",
    "Weab-chan/freedreno_turnip-CI"
)

private data class DriverAsset(val release: String, val name: String, val url: String, val size: Long)

private fun openDriverConnection(url: String, json: Boolean): HttpURLConnection {
    val c = URL(url).openConnection() as HttpURLConnection
    c.connectTimeout = 15000
    c.readTimeout = 30000
    c.setRequestProperty("User-Agent", "Vita3K-Plus")
    if (json) c.setRequestProperty("Accept", "application/vnd.github+json")
    return c
}

private fun fetchDriverAssets(repo: String): List<DriverAsset> {
    val c = openDriverConnection("https://api.github.com/repos/$repo/releases?per_page=15", true)
    try {
        if (c.responseCode == 403 || c.responseCode == 429) error("GitHub rate limit reached, try again later.")
        if (c.responseCode != 200) error("GitHub returned HTTP ${c.responseCode}.")
        val releases = JSONArray(c.inputStream.bufferedReader().use { it.readText() })
        val out = ArrayList<DriverAsset>()
        for (i in 0 until releases.length()) {
            val r = releases.getJSONObject(i)
            val title = r.optString("name").ifBlank { r.optString("tag_name") }
            val assets = r.optJSONArray("assets") ?: continue
            for (j in 0 until assets.length()) {
                val a = assets.getJSONObject(j)
                val name = a.optString("name")
                if (name.endsWith(".zip", ignoreCase = true)) {
                    out.add(DriverAsset(title, name, a.getString("browser_download_url"), a.optLong("size")))
                }
            }
        }
        return out
    } finally {
        c.disconnect()
    }
}

private fun downloadDriver(context: Context, asset: DriverAsset, onProgress: (Float) -> Unit): File {
    val dir = File(context.cacheDir, "drivers").apply { mkdirs() }
    dir.listFiles()?.forEach { it.delete() }
    val file = File(dir, asset.name.replace(Regex("[^A-Za-z0-9._-]"), "_"))
    val c = openDriverConnection(asset.url, false)
    try {
        if (c.responseCode != 200) error("Download failed (HTTP ${c.responseCode}).")
        val total = if (c.contentLengthLong > 0) c.contentLengthLong else asset.size
        var done = 0L
        var lastPercent = -1
        c.inputStream.use { input ->
            file.outputStream().use { output ->
                val buf = ByteArray(64 * 1024)
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) break
                    output.write(buf, 0, n)
                    done += n
                    if (total > 0) {
                        val percent = (done * 100 / total).toInt().coerceIn(0, 100)
                        if (percent != lastPercent) {
                            lastPercent = percent
                            onProgress(percent / 100f)
                        }
                    }
                }
            }
        }
        return file
    } finally {
        c.disconnect()
    }
}

@Composable
fun DriverFetchDialog(onDismiss: () -> Unit, onInstall: (String) -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var repoIndex by remember { mutableStateOf(0) }
    var assets by remember { mutableStateOf<List<DriverAsset>>(emptyList()) }
    var loading by remember { mutableStateOf(true) }
    var message by remember { mutableStateOf<String?>(null) }
    var progress by remember { mutableStateOf<Float?>(null) }
    var busyName by remember { mutableStateOf("") }

    LaunchedEffect(repoIndex) {
        loading = true
        message = null
        assets = emptyList()
        try {
            assets = withContext(Dispatchers.IO) { fetchDriverAssets(DRIVER_REPOS[repoIndex]) }
            if (assets.isEmpty()) message = "No driver .zip found in this repo."
        } catch (e: Exception) {
            message = e.message ?: "Failed to load the driver list."
        }
        loading = false
    }

    fun startDownload(asset: DriverAsset) {
        if (progress != null) return
        busyName = asset.name
        progress = 0f
        message = null
        scope.launch {
            try {
                val file = withContext(Dispatchers.IO) {
                    downloadDriver(context, asset) { progress = it }
                }
                onInstall(file.absolutePath)
            } catch (e: Exception) {
                progress = null
                message = e.message ?: "Download failed."
            }
        }
    }

    AlertDialog(
        onDismissRequest = { if (progress == null) onDismiss() },
        title = { Text("Download GPU driver") },
        text = {
            Column {
                Row(
                    Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()),
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    DRIVER_REPOS.forEachIndexed { i, repo ->
                        val label = repo.substringBefore('/')
                        val pick = { if (progress == null) repoIndex = i }
                        if (i == repoIndex) {
                            Button(onClick = pick) { Text(label) }
                        } else {
                            OutlinedButton(onClick = pick) { Text(label) }
                        }
                    }
                }
                Spacer(Modifier.height(12.dp))
                val p = progress
                val m = message
                if (p != null) {
                    Text("Downloading $busyName... ${(p * 100).toInt()}%")
                    Spacer(Modifier.height(8.dp))
                    LinearProgressIndicator(progress = { p }, modifier = Modifier.fillMaxWidth())
                } else if (loading) {
                    LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                }
                if (m != null) {
                    Text(m, color = MaterialTheme.colorScheme.error)
                }
                if (p == null && !loading) {
                    LazyColumn(Modifier.heightIn(max = 320.dp)) {
                        items(assets) { a ->
                            Column(
                                Modifier.fillMaxWidth().clickable { startDownload(a) }.padding(vertical = 8.dp)
                            ) {
                                Text(a.name)
                                Text(
                                    "${a.release} - ${"%.1f".format(a.size / 1048576.0)} MB",
                                    style = MaterialTheme.typography.bodySmall
                                )
                            }
                        }
                    }
                }
            }
        },
        confirmButton = {},
        dismissButton = {
            TextButton(onClick = onDismiss, enabled = progress == null) { Text("Close") }
        }
    )
}
