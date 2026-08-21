package dev.primolab.miner

import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.InetSocketAddress
import java.net.Socket

/**
 * Minimal client for the miner's ccminer-compatible read-only API
 * (default 127.0.0.1:4068). Commands are newline-terminated; responses are
 * a single pipe-delimited line of KEY=VALUE pairs, e.g.
 *   NAME=...;ALGO=verus;KHS=5510.00;ACC=12;REJ=0;...|
 */
object ApiClient {
    const val HOST = "127.0.0.1"
    const val PORT = 4068

    /** Send one command, return KEY=VALUE map of the first record, or null if unreachable. */
    fun query(command: String): Map<String, String>? {
        return try {
            Socket().use { sock ->
                sock.connect(InetSocketAddress(HOST, PORT), 1500)
                sock.soTimeout = 1500
                OutputStreamWriter(sock.getOutputStream()).apply {
                    write(command); write("\n"); flush()
                }
                val line = BufferedReader(InputStreamReader(sock.getInputStream())).readLine()
                    ?: return null
                parseRecord(line)
            }
        } catch (_: Exception) {
            null
        }
    }

    fun summary(): Map<String, String>? = query("summary")
    fun hwinfo(): Map<String, String>? = query("hwinfo")

    /** Multi-record commands (e.g. "threads") — one map per '|'-delimited record. */
    fun queryRecords(command: String): List<Map<String, String>>? {
        return try {
            Socket().use { sock ->
                sock.connect(InetSocketAddress(HOST, PORT), 1500)
                sock.soTimeout = 1500
                OutputStreamWriter(sock.getOutputStream()).apply {
                    write(command); write("\n"); flush()
                }
                val line = BufferedReader(InputStreamReader(sock.getInputStream())).readLine()
                    ?: return null
                line.split('|').filter { it.isNotBlank() }.map { parseFields(it) }
            }
        } catch (_: Exception) {
            null
        }
    }

    fun threads(): List<Map<String, String>>? = queryRecords("threads")

    /** Parse the first '|'-delimited record into a KEY=VALUE map. */
    private fun parseRecord(line: String): Map<String, String> = parseFields(line.substringBefore('|'))

    private fun parseFields(record: String): Map<String, String> =
        record.split(';')
            .mapNotNull { field ->
                val i = field.indexOf('=')
                if (i <= 0) null else field.substring(0, i) to field.substring(i + 1)
            }
            .toMap()
}
