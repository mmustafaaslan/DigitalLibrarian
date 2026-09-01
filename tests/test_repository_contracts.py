"""Fast, non-destructive repository checks for safety-critical contracts."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
SKETCH = (ROOT / "DigitalLibrarian.ino").read_text(encoding="utf-8")
STORAGE = (ROOT / "Storage.cpp").read_text(encoding="utf-8")
UI = (ROOT / "UIManager.cpp").read_text(encoding="utf-8")
WORKER = (ROOT / "BackgroundWorker.cpp").read_text(encoding="utf-8")
NETWORK = (ROOT / "NetworkManager.cpp").read_text(encoding="utf-8")
MEDIA = (ROOT / "MediaManager.cpp").read_text(encoding="utf-8")
GLOBALS = (ROOT / "AppGlobals.cpp").read_text(encoding="utf-8")
UTILS = (ROOT / "Utils.cpp").read_text(encoding="utf-8")
TOUCH_PORT = (ROOT / "Waveshare_ST7262_LVGL.cpp").read_text(encoding="utf-8")


class RepositoryContracts(unittest.TestCase):
    def test_mutating_routes_are_post_only(self):
        for route in ("/api/control", "/api/setcover", "/api/lookup", "/restart"):
            self.assertIn(f'server.on("{route}", HTTP_POST', SKETCH)

    def test_destructive_storage_tests_are_compile_time_gated(self):
        self.assertIn("ENABLE_DESTRUCTIVE_STORAGE_TESTS", SKETCH)
        self.assertIn('server.on("/api/tests/run", HTTP_POST', SKETCH)

    def test_generated_pages_do_not_put_pin_in_urls(self):
        self.assertNotIn("?pin=", SKETCH)
        self.assertNotIn("searchParams.set('pin'", SKETCH)

    def test_auth_cookie_does_not_contain_the_pin(self):
        self.assertIn('"DL_AUTH=" + webSessionToken', SKETCH)
        self.assertNotIn('"DL_AUTH=" + web_pin', SKETCH)
        self.assertIn("webRequestAuthorized(bool allowFormPin = false)", SKETCH)

    def test_storage_writes_use_temporary_files(self):
        self.assertIn('String tmpPath = path + ".tmp";', STORAGE)
        self.assertIn("replaceFileWithRollback", STORAGE)

    def test_sd_mount_failure_is_retried_and_never_shown_as_empty_library(self):
        self.assertIn("bool mountLibrarySdCard()", SKETCH)
        self.assertIn("frequencies[] = {4000000, 2000000, 1000000}", SKETCH)
        self.assertIn("SD.end()", SKETCH)
        self.assertIn("SD.cardType() != CARD_NONE", SKETCH)
        self.assertIn("sd_card_ready = mountLibrarySdCard()", SKETCH)
        self.assertIn('"SD card unavailable"', UI)
        self.assertIn("no data was erased", UI)

        save_handler = UI[UI.index("void perform_save_item() {"):]
        save_handler = save_handler[:save_handler.index("void save_new_item_cb")]
        self.assertIn("if (!sd_card_ready)", save_handler)
        self.assertLess(
            save_handler.index("if (!sd_card_ready)"),
            save_handler.index("BackgroundWorker::addJob"),
        )

    def test_detail_updates_roll_back_when_index_save_fails(self):
        self.assertGreaterEqual(STORAGE.count("rollbackReplacedFile(path, hadDetail)"), 3)
        self.assertIn("IndexVector updated = vec", STORAGE)

    def test_track_summary_sd_work_runs_in_background_worker(self):
        self.assertIn("case JOB_TRACK_SUMMARY_LOAD", WORKER)
        summary_scheduler = UI[UI.index("static void schedule_track_summary_load"):]
        summary_scheduler = summary_scheduler[:summary_scheduler.index("// Helper functions")]
        self.assertNotIn("Storage.loadTracklist", summary_scheduler)

    def test_web_random_action_takes_lvgl_lock(self):
        random_action = SKETCH[SKETCH.index('if (action == "random")'):]
        random_action = random_action[:random_action.index('} else if (action == "select")')]
        self.assertIn("lvgl_port_lock", random_action)
        self.assertIn("lvgl_port_unlock", random_action)

    def test_cover_reads_are_verified(self):
        self.assertIn("bytesRead != jpg_size", UI)
        self.assertIn("MALLOC_CAP_SPIRAM", UI)

    def test_https_clients_verify_certificates(self):
        self.assertNotIn("setInsecure()", NETWORK)
        self.assertNotIn("setInsecure()", MEDIA)
        trust = (ROOT / "TlsTrust.h").read_text(encoding="utf-8")
        self.assertIn("setCACertBundle", trust)
        self.assertIn("configTime", trust)

    def test_led_count_has_hard_bounds(self):
        globals_header = (ROOT / "AppGlobals.h").read_text(encoding="utf-8")
        self.assertRegex(globals_header, r"LED_MIN_COUNT\s*=\s*1")
        self.assertRegex(globals_header, r"LED_MAX_COUNT\s*=\s*1200")

    def test_home_brand_text_is_persisted_and_not_hard_coded_in_main_ui(self):
        self.assertIn('getString("brand_title", DEFAULT_BRAND_TITLE)', GLOBALS)
        self.assertIn('getString("brand_sub", DEFAULT_BRAND_SUBTITLE)', GLOBALS)
        self.assertIn('putString("brand_title", setting_brand_title)', GLOBALS)
        self.assertIn('putString("brand_sub", setting_brand_subtitle)', GLOBALS)
        self.assertIn("sanitizeLvglText(setting_brand_title).c_str()", UI)
        self.assertIn("sanitizeLvglText(setting_brand_subtitle).c_str()", UI)

    def test_lvgl_text_is_normalized_without_changing_cached_source_data(self):
        self.assertIn("void sanitizeLvglTextInPlace(PsramString &text)", UTILS)
        self.assertIn("decodeUtf8(text.data(), text.size(), offset)", UTILS)
        self.assertIn("sanitizeLvglTextInPlace(popupData->lyrics)", UI)
        self.assertIn("lv_label_set_text_static(chunkLabel", UI)
        self.assertNotIn('labelStr += "  —  "', UI)
        self.assertNotIn('labelStr += "  •  "', UI)
        self.assertIn('labelStr += "  -  "', UI)
        self.assertIn('labelStr += "  |  "', UI)

    def test_delete_cover_action_requires_explicit_cover_tap(self):
        cover_setup = UI[UI.index("img_cover = lv_img_create"):]
        cover_setup = cover_setup[:cover_setup.index("// CD Info Container")]
        self.assertIn("lv_obj_add_flag(btn_delete_cover, LV_OBJ_FLAG_HIDDEN)", cover_setup)
        self.assertIn("lv_obj_clear_flag(btn_delete_cover, LV_OBJ_FLAG_HIDDEN)", cover_setup)
        self.assertNotIn("lv_obj_has_flag(btn_delete_cover", cover_setup)

        delete_handler = UI[UI.index("void btn_delete_cover_clicked"):]
        delete_handler = delete_handler[:delete_handler.index("void btn_search_clicked")]
        self.assertIn("lv_obj_add_flag(btn_delete_cover, LV_OBJ_FLAG_HIDDEN)", delete_handler)

    def test_wifi_scroll_content_stays_behind_fixed_header(self):
        wifi_ui = UI[UI.index("void show_wifi_config_ui() {"):]
        wifi_ui = wifi_ui[:wifi_ui.index("void close_wifi_config_ui() {")]
        self.assertIn("lv_obj_set_style_bg_opa(fixed_header, LV_OPA_COVER", wifi_ui)
        self.assertIn("lv_obj_move_foreground(fixed_header)", wifi_ui)
        self.assertIn("lv_obj_move_foreground(wifi_connect_button)", wifi_ui)
        self.assertIn("lv_obj_move_foreground(btn_toggle_kb_wifi)", wifi_ui)
        self.assertIn("lv_obj_move_foreground(btn_close)", wifi_ui)

    def test_wifi_page_waits_for_real_connection_result(self):
        wifi_ui = UI[UI.index("void show_wifi_config_ui() {"):]
        wifi_ui = wifi_ui[:wifi_ui.index("void close_wifi_config_ui() {")]
        self.assertIn("WIFI_CONNECT_TIMEOUT_TICKS = 60", UI)
        self.assertIn("WiFi.mode(WIFI_STA)", wifi_ui)
        self.assertIn("status == WL_NO_SSID_AVAIL", wifi_ui)
        self.assertIn("status == WL_CONNECT_FAILED", wifi_ui)
        self.assertNotIn("Connection failed. Check credentials.", wifi_ui)

    def test_optional_second_wifi_is_seeded_once_without_tracking_secrets(self):
        self.assertIn("WIFI_CREDENTIAL_SEED_VERSION", NETWORK)
        self.assertIn('getUChar("seed_ver", 0)', NETWORK)
        self.assertIn('putUChar("seed_ver", WIFI_CREDENTIAL_SEED_VERSION)', NETWORK)
        self.assertIn("#if defined(WIFI_SSID_2) && defined(WIFI_PASSWORD_2)", NETWORK)
        self.assertIn("#if defined(WIFI_SSID_3) && defined(WIFI_PASSWORD_3)", NETWORK)
        gitignore = (ROOT / ".gitignore").read_text(encoding="utf-8")
        self.assertRegex(gitignore, r"(?m)^secrets\.h$")

    def test_saved_wifi_fallback_waits_and_reports_live_progress(self):
        self.assertIn("SAVED_NETWORK_ATTEMPT_TIMEOUT_MS = 30000", NETWORK)
        self.assertIn("getConnectionNetworkIndex", NETWORK)
        self.assertIn("WiFi.setAutoReconnect(false)", NETWORK)
        self.assertIn("WiFi.setAutoReconnect(true)", NETWORK)
        self.assertIn("Trying saved network %d of %d: %s", UI)
        self.assertIn("refresh_wifi_indicators", UI)

        legacy_connect = NETWORK[NETWORK.index(
            "bool AppNetworkManager::tryConnectToSavedNetworks()"
        ):]
        legacy_connect = legacy_connect[:legacy_connect.index(
            "void AppNetworkManager::startConnection()"
        )]
        self.assertNotIn("while (WiFi.status()", legacy_connect)
        self.assertNotIn("delay(500)", legacy_connect)

    def test_saved_wifi_rows_can_trigger_manual_connection(self):
        wifi_ui = UI[UI.index("void show_wifi_config_ui() {"):]
        wifi_ui = wifi_ui[:wifi_ui.index("void close_wifi_config_ui() {")]
        self.assertIn("lv_obj_add_flag(net_container, LV_OBJ_FLAG_CLICKABLE)", wifi_ui)
        self.assertIn("savedWiFiNetworks[index].password.c_str()", wifi_ui)
        self.assertIn("lv_event_send(wifi_connect_button, LV_EVENT_CLICKED", wifi_ui)
        self.assertIn("AppNetworkManager::cancelConnectionAttempts()", wifi_ui)
        self.assertIn("AppNetworkManager::completeManualConnection()", wifi_ui)
        self.assertIn("lv_obj_set_size(btn_delete, 36, 36)", wifi_ui)
        self.assertIn("lv_obj_set_ext_click_area(btn_delete, 4)", wifi_ui)

    def test_cover_search_never_blocks_the_lvgl_event_handler(self):
        cover_handler = UI[UI.index("void btn_search_clicked(lv_event_t *e) {"):]
        cover_handler = cover_handler[:cover_handler.index("void close_search_ui")]
        self.assertIn("JOB_COVER_DOWNLOAD", cover_handler)
        self.assertIn("BackgroundWorker::addJob", cover_handler)
        self.assertNotIn("downloadCoverImage", cover_handler)
        self.assertNotIn("fetchAlbumCoverUrl", cover_handler)
        self.assertIn("case JOB_COVER_DOWNLOAD", WORKER)
        self.assertIn("fetchCoverUrlForIndex(currentJob.index)", WORKER)

    def test_network_progress_has_a_live_activity_indicator(self):
        progress_monitor = UI[UI.index("// --- Progress Monitor Timer ---"):]
        progress_monitor = progress_monitor[:progress_monitor.index(
            'Serial.println(">> setupMainUI Done")'
        )]
        self.assertIn("lv_spinner_create", progress_monitor)
        self.assertIn("the app has not frozen", progress_monitor)
        self.assertIn("BackgroundWorker::getStatusMessage()", progress_monitor)

    def test_metadata_lookup_and_lyrics_use_the_background_worker(self):
        metadata_handler = UI[UI.index("void fetch_barcode_cb"):]
        metadata_handler = metadata_handler[:metadata_handler.index(
            "void close_add_item_ui"
        )]
        self.assertIn("JOB_METADATA_LOOKUP", metadata_handler)
        self.assertNotIn("fetchModeMetadata", metadata_handler)
        self.assertIn("case JOB_METADATA_LOOKUP", WORKER)
        self.assertIn("case JOB_LYRICS_FETCH_ONE", WORKER)
        self.assertIn("case JOB_LYRICS_FETCH_ALL", WORKER)

    def test_slow_network_operations_are_cooperatively_cancellable(self):
        worker_header = (ROOT / "BackgroundWorker.h").read_text(
            encoding="utf-8"
        )
        self.assertIn("static void requestCancel()", worker_header)
        self.assertIn("std::atomic<bool> _cancelRequested", worker_header)
        self.assertIn("static void requestSkipCurrent()", worker_header)
        self.assertIn("std::atomic<bool> _skipRequested", worker_header)
        self.assertIn("BackgroundWorker::requestCancel()", UI)
        self.assertIn("Cancelling at the next safe checkpoint", UI)
        self.assertIn(
            'setStatus("Cancelling at the next safe checkpoint..."',
            WORKER,
        )
        self.assertIn('lv_label_set_text_static(label, "CANCELLING")', UI)
        self.assertIn("isCancellationRequested()", MEDIA)

        response_reader = MEDIA[MEDIA.index(
            "bool readHttpResponseToPsram"
        ):MEDIA.index("bool extractLyricsFromResponse")]
        self.assertIn("networkAbortRequested()", response_reader)
        self.assertIn("body.clear()", response_reader)
        abort_helper = MEDIA[MEDIA.index("bool networkAbortRequested()"):]
        abort_helper = abort_helper[:abort_helper.index(
            "bool readHttpResponseToPsram"
        )]
        self.assertIn("isCancellationRequested()", abort_helper)
        self.assertIn("isSkipRequested()", abort_helper)

        metadata_handler = UI[UI.index("void fetch_barcode_cb"):]
        metadata_handler = metadata_handler[:metadata_handler.index(
            "void close_add_item_ui"
        )]
        self.assertIn(
            '"Please wait; the interface is responsive.", true',
            metadata_handler,
        )

        save_handler = UI[UI.index("void perform_save_item() {"):]
        save_handler = save_handler[:save_handler.index(
            "void save_new_item_cb"
        )]
        self.assertIn("Writing and verifying the SD card", save_handler)
        self.assertNotIn("Writing and verifying the SD card; controls remain responsive.\", true", save_handler)

        bulk_sync = WORKER[WORKER.index("case JOB_BULK_SYNC"):]
        bulk_sync = bulk_sync[:bulk_sync.index("case JOB_COVER_DOWNLOAD")]
        self.assertIn("isCancellationRequested()", bulk_sync)
        self.assertIn("_skipRequested.exchange", bulk_sync)
        self.assertIn("!isSkipRequested()", bulk_sync)
        self.assertIn("Sync stopped: WiFi disconnected", bulk_sync)
        self.assertIn("String(i + 1)", bulk_sync)
        self.assertIn("item.coverUrl", bulk_sync)

        cover_download = NETWORK[NETWORK.index(
            "bool AppNetworkManager::downloadCoverImage"
        ):NETWORK.index("void AppNetworkManager::forceUpdateWLED")]
        self.assertIn("BackgroundWorker::isCancellationRequested()", cover_download)
        self.assertIn("BackgroundWorker::isSkipRequested()", cover_download)
        self.assertIn("bool quickMode", cover_download)
        self.assertIn("quickMode ? 3000 : 8000", cover_download)
        self.assertIn("quickMode ? 3UL : 8UL", cover_download)
        self.assertIn(
            "setHandshakeTimeout(handshakeTimeoutSeconds)", cover_download
        )
        self.assertIn("quickMode ? 7000 : 15000", cover_download)
        self.assertIn("quickMode ? 2500 : 5000", cover_download)
        self.assertIn("millis() - startedAt < streamTimeoutMs", cover_download)
        self.assertIn("millis() - lastDataAt < idleTimeoutMs", cover_download)
        self.assertIn(
            "downloadCoverImage(downloadUrl, savePath,\n                                                        true)",
            bulk_sync,
        )

        generic_progress = UI[UI.index("// --- Progress Monitor Timer ---"):]
        self.assertIn("progressJob == JOB_BULK_SYNC", generic_progress)
        self.assertIn("Weak WiFi can slow individual records", generic_progress)
        self.assertIn("BackgroundWorker::requestSkipCurrent()", generic_progress)
        self.assertIn('lv_label_set_text_static(progress_skip_label, "SKIP ITEM")', generic_progress)

        # Skip/cancel is observed by the network worker at safe checkpoints.
        # Closing its live TLS client from the UI core can race the ESP32
        # networking stack and reboot the board.
        self.assertNotIn("abortActiveRequest", WORKER)
        self.assertNotIn("activeRequestClient->stop()", NETWORK)
        self.assertNotIn("ScopedActiveRequest", NETWORK)
        self.assertNotIn("ScopedActiveMediaRequest", MEDIA)

        # ESP32 NetworkClientSecure takes handshake timeouts in seconds.
        # Millisecond-looking values here made Skip appear stuck for minutes.
        self.assertNotRegex(
            MEDIA + NETWORK,
            r"setHandshakeTimeout\((?:2000|3000|8000|10000|15000)\)",
        )

    def test_item_save_transaction_never_runs_in_touch_callback(self):
        save_handler = UI[UI.index("void perform_save_item() {"):]
        save_handler = save_handler[:save_handler.index(
            "void save_new_item_cb"
        )]
        self.assertIn("JOB_ITEM_SAVE", save_handler)
        self.assertIn("BackgroundWorker::addJob(saveJob)", save_handler)
        self.assertNotIn("saveCurrentEditItem", save_handler)
        self.assertNotIn("Storage.saveCD", save_handler)
        self.assertNotIn("Storage.saveBook", save_handler)
        self.assertIn("getItemAtRAM(i)", save_handler)

        save_job = WORKER[WORKER.index("case JOB_ITEM_SAVE"):]
        save_job = save_job[:save_job.index("case JOB_BULK_SYNC")]
        self.assertIn("saveItemViewToStorage", save_job)
        self.assertIn("delete payload", save_job)
        self.assertIn("takeItemSaveCompletion", UI)
        self.assertIn("initNavigationCache()", UI)

    def test_offline_network_actions_fail_before_queueing(self):
        track_handler = UI[UI.index("static void trackClickHandler"):]
        track_handler = track_handler[:track_handler.index(
            "static void append_tracklist_row"
        )]
        self.assertIn("WiFi.status() != WL_CONNECTED", track_handler)
        self.assertLess(
            track_handler.index("WiFi.status() != WL_CONNECTED"),
            track_handler.index("BackgroundWorker::addJob"),
        )

        metadata_handler = UI[UI.index("void fetch_barcode_cb"):]
        metadata_handler = metadata_handler[:metadata_handler.index(
            "void close_add_item_ui"
        )]
        self.assertIn("WiFi.status() != WL_CONNECTED", metadata_handler)
        self.assertLess(
            metadata_handler.index("WiFi.status() != WL_CONNECTED"),
            metadata_handler.index("BackgroundWorker::addJob"),
        )

        self.assertGreaterEqual(
            WORKER.count("No WiFi connection. Connect and try again."), 3
        )

    def test_song_taps_do_not_read_sd_or_rebuild_panels_on_ui_thread(self):
        track_handler = UI[UI.index("static void trackClickHandler"):]
        track_handler = track_handler[:track_handler.index(
            "static void append_tracklist_row"
        )]
        self.assertNotIn("Storage.loadLyrics", track_handler)
        self.assertIn("JOB_LYRICS_FETCH_ONE", track_handler)
        self.assertIn("JOB_LYRICS_LOAD_CACHED", track_handler)
        self.assertIn("tracklist_feedback_label", track_handler)
        self.assertIn("lyrics_request_pending", track_handler)
        self.assertIn("returned to LVGL", track_handler)
        self.assertIn("takeLyricsCompletion", UI)
        self.assertIn("Storage.loadLyrics(lyricsPath.c_str(), lyricsText)", WORKER)

        progress_monitor = UI[UI.index("// --- Progress Monitor Timer ---"):]
        progress_monitor = progress_monitor[:progress_monitor.index(
            'Serial.println(">> setupMainUI Done")'
        )]
        self.assertLess(
            progress_monitor.index("takeLyricsCompletion"),
            progress_monitor.index("// Create/Show Modal"),
        )
        self.assertIn("dismiss_progress_modal", progress_monitor)
        self.assertIn("lyrics_request_pending = false", progress_monitor)
        self.assertNotIn("close_tracklist_ui", progress_monitor)
        self.assertNotIn("show_tracklist_ui", progress_monitor)

        cached_request = track_handler[
            track_handler.index("JOB_LYRICS_LOAD_CACHED"):
            track_handler.index("JOB_LYRICS_FETCH_ONE")
        ]
        self.assertIn("nullptr, false", cached_request)

        uncached_request = track_handler[
            track_handler.index("if (!isCached) {"):
        ]
        self.assertIn("close_tracklist_ui()", uncached_request)
        self.assertIn("show_compact_lyrics_progress()", uncached_request)
        self.assertLess(
            uncached_request.index("close_tracklist_ui()"),
            uncached_request.index("BackgroundWorker::addJob"),
        )
        self.assertIn("120, deferredJob", uncached_request)
        self.assertLess(
            uncached_request.index("BackgroundWorker::addJob"),
            uncached_request.index("show_info_popup"),
        )

        progress_monitor = UI[UI.index("// --- Progress Monitor Timer ---"):]
        self.assertIn("if (progress_bar)", progress_monitor)
        self.assertIn("!compact_tls_progress_active", progress_monitor)

    def test_metadata_fetch_is_psram_backed_and_only_stages_edits(self):
        release_lookup = MEDIA[MEDIA.index(
            "MBRelease MediaManager::fetchReleaseByBarcode"
        ):MEDIA.index("// Discogs API Fallback")]
        discogs_lookup = MEDIA[MEDIA.index(
            "MBRelease MediaManager::fetchReleaseFromDiscogs"
        ):MEDIA.index("std::vector<Track> MediaManager::fetchTracklist")]
        book_lookup = MEDIA[MEDIA.index(
            "bool MediaManager::fetchBookByISBN"
        ):MEDIA.index("// Metadata Fetching (Online)")]
        cd_metadata = MEDIA[MEDIA.index(
            "bool MediaManager::fetchMetadataForBarcode"
        ):MEDIA.index("bool MediaManager::fetchMetadataForISBN")]
        book_metadata = MEDIA[MEDIA.index(
            "bool MediaManager::fetchMetadataForISBN"
        ):MEDIA.index("String MediaManager::fetchAlbumCoverUrl")]

        for lookup in (release_lookup, discogs_lookup, book_lookup):
            self.assertIn("readHttpResponseToPsram", lookup)
            self.assertNotIn("http.getString()", lookup)
        self.assertNotIn("Storage.saveCD(cd)", cd_metadata)
        self.assertNotIn("Storage.saveBook(book)", book_metadata)
        self.assertIn("getItemAtSD(currentJob.index)", WORKER)
        self.assertIn('startsWith("discogs_")', cd_metadata)
        self.assertIn("BackgroundWorker::reportProgress", MEDIA)
        self.assertIn("client.setTimeout(15000)", MEDIA)
        self.assertIn("http.setTimeout(15000)", MEDIA)
        self.assertNotIn(
            "Fetching genre from Discogs to supplement", release_lookup
        )

        metadata_handler = UI[UI.index("void fetch_barcode_cb"):]
        metadata_handler = metadata_handler[:metadata_handler.index(
            "void close_add_item_ui"
        )]
        self.assertIn("show_compact_network_progress(", metadata_handler)
        self.assertIn("progressTitle.c_str(), true", metadata_handler)
        self.assertIn("Checking MusicBrainz...", UI)
        self.assertIn("Loading songs and duration...", UI)
        progress_update = UI[UI.index("// Update"):UI.index("// Close")]
        self.assertLess(
            progress_update.index("BackgroundWorker::getProgress()"),
            progress_update.index("if (progress_bar)"),
        )
        self.assertGreater(
            progress_update.index("compact_metadata_progress_active"),
            progress_update.index("if (progress_bar)"),
        )

    def test_tracklist_json_buffer_outlives_zero_copy_parse(self):
        track_fetch = MEDIA[MEDIA.index(
            "std::vector<Track> MediaManager::fetchTracklist"
        ):MEDIA.index("bool MediaManager::fetchBookByISBN")]
        self.assertIn("PsramString response", track_fetch)
        self.assertIn("deserializeJson(doc, response.data(), response.size())",
                      track_fetch)
        self.assertNotIn("free(psBuffer)", track_fetch)

    def test_opening_song_list_never_reads_or_parses_sd_on_ui_thread(self):
        open_handler = UI[UI.index("void show_tracklist_ui(int idx) {"):]
        open_handler = open_handler[:open_handler.index(
            "// show_chapter_list_ui removed"
        )]
        self.assertIn("JOB_TRACKLIST_LOAD", open_handler)
        self.assertIn("BackgroundWorker::addJob", open_handler)
        self.assertNotIn("Storage.loadTracklist", open_handler)
        self.assertNotIn("ensureItemDetailsLoaded", open_handler)

        worker_job = WORKER[WORKER.index("case JOB_TRACKLIST_LOAD"):]
        worker_job = worker_job[:worker_job.index(
            "case JOB_LYRICS_LOAD_CACHED"
        )]
        self.assertIn("Storage.loadCDDetail", worker_job)
        self.assertIn("Storage.loadTracklist", worker_job)
        self.assertIn("No readable track list", worker_job)

        progress_monitor = UI[UI.index("// --- Progress Monitor Timer ---"):]
        progress_monitor = progress_monitor[:progress_monitor.index(
            'Serial.println(">> setupMainUI Done")'
        )]
        self.assertLess(
            progress_monitor.index("takeTracklistCompletion"),
            progress_monitor.index("// Create/Show Modal"),
        )
        self.assertIn("render_tracklist_ui", progress_monitor)
        self.assertIn("TRACKS_PER_PAGE = 4", UI)
        self.assertIn("lv_obj_clean(state->container)", UI)
        self.assertNotIn("ROWS_PER_TICK", UI)

    def test_lyrics_transition_preserves_ui_core_and_static_text_lifetime(self):
        display_h = (ROOT / "Waveshare_ST7262_LVGL.h").read_text(
            encoding="utf-8", errors="replace"
        )
        self.assertIn("LVGL_PORT_TASK_CORE", display_h)
        self.assertRegex(display_h, r"LVGL_PORT_TASK_CORE\s+\\\s*\n\s*\(1\)")
        self.assertIn("xTaskCreateStaticPinnedToCore", WORKER)
        self.assertIn("WORKER_STACK_BYTES = 32768", WORKER)
        self.assertIn("MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT", WORKER)
        self.assertIn('_taskStackBuffer, &_taskControlBlock, 0', WORKER)

        lyrics_popup = UI[UI.index("void close_lyrics_popup"):]
        lyrics_popup = lyrics_popup[:lyrics_popup.index("static void trackClickHandler")]
        self.assertIn("lv_obj_del_async(panelToDelete)", lyrics_popup)
        self.assertIn("lv_obj_t *dataOwner", lyrics_popup)
        self.assertIn("LV_EVENT_DELETE, popupData", lyrics_popup)
        owner_pos = lyrics_popup.index("lv_obj_t *dataOwner")
        label_pos = lyrics_popup.index("lv_label_set_text_static")
        self.assertGreater(owner_pos, label_pos)

    def test_rgb_timing_uses_panel_validated_clock(self):
        board_config = (ROOT / "ESP_Panel_Board_Custom.h").read_text(
            encoding="utf-8", errors="replace"
        )
        self.assertIn("ESP_PANEL_LCD_RGB_CLK_HZ (16 * 1000 * 1000)", board_config)
        display_h = (ROOT / "Waveshare_ST7262_LVGL.h").read_text(
            encoding="utf-8", errors="replace"
        )
        self.assertIn("LVGL_PORT_RGB_BOUNCE_BUFFER_SIZE", display_h)
        self.assertIn("LVGL_PORT_DISP_WIDTH * 15", display_h)

    def test_track_parsing_releases_shared_touch_bus_and_touch_fails_safe(self):
        track_loader = STORAGE[STORAGE.index(
            "TrackList *LibrarianStorage::loadTracklist"
        ):]
        track_loader = track_loader[:track_loader.index(
            "bool LibrarianStorage::saveTracklist"
        )]
        self.assertIn("Parse after ScopedSdAccess has released", track_loader)
        self.assertIn("heap_caps_malloc", track_loader)
        self.assertIn("deserializeJson(doc, jsonBuffer, jsonSize)", track_loader)

        touch_reader = TOUCH_PORT[TOUCH_PORT.index("static void touchpad_read"):]
        touch_reader = touch_reader[:touch_reader.index("static lv_indev_t *indev_init")]
        self.assertIn("suppress_until_release", touch_reader)
        self.assertIn("missed_reads >= 2", touch_reader)
        self.assertIn("last_pressed = false", touch_reader)

    def test_lyrics_stay_in_psram_and_render_in_bounded_chunks(self):
        lyrics_loader = STORAGE[STORAGE.index(
            "bool LibrarianStorage::loadLyrics"
        ):]
        lyrics_loader = lyrics_loader[:lyrics_loader.index(
            "bool LibrarianStorage::saveLyrics"
        )]
        self.assertIn("PsramString &lyricsOut", lyrics_loader)
        self.assertIn("MALLOC_CAP_SPIRAM", lyrics_loader)
        self.assertIn("BasicJsonDocument<SpiRamAllocator>", lyrics_loader)

        lyrics_popup = UI[UI.index("struct LyricsPopupData"):]
        lyrics_popup = lyrics_popup[:lyrics_popup.index(
            "static void trackClickHandler"
        )]
        self.assertIn("PsramString lyrics", lyrics_popup)
        self.assertIn("LYRICS_CHUNK_CHARS = 900", lyrics_popup)
        self.assertIn("lv_label_set_text_static", lyrics_popup)
        self.assertNotIn("lv_label_set_text(lblLyrics", lyrics_popup)
        self.assertIn("lv_obj_add_flag(lyrics_panel, LV_OBJ_FLAG_HIDDEN)", lyrics_popup)
        self.assertIn("lv_obj_update_layout(lyrics_panel)", lyrics_popup)
        self.assertIn("lv_obj_clear_flag(lyrics_panel, LV_OBJ_FLAG_HIDDEN)", lyrics_popup)

        cached_job = WORKER[WORKER.index("case JOB_LYRICS_LOAD_CACHED"):]
        cached_job = cached_job[:cached_job.index("case JOB_LYRICS_FETCH_ONE")]
        self.assertIn("Storage.loadLyrics(currentJob.id.c_str()", cached_job)
        self.assertNotIn("Storage.loadTracklist", cached_job)

        lyrics_fetch = MEDIA[MEDIA.index("LyricsResult fetchLyricsIfNeeded"):]
        lyrics_fetch = lyrics_fetch[:lyrics_fetch.index(
            "void fetchAllLyrics"
        )]
        self.assertIn("PsramString finalLyrics", lyrics_fetch)
        self.assertIn("extractLyricsFromResponse", lyrics_fetch)
        self.assertNotIn("http.getString()", lyrics_fetch)
        self.assertNotIn("DynamicJsonDocument", lyrics_fetch)

        lyrics_saver = STORAGE[STORAGE.index(
            "bool LibrarianStorage::saveLyrics"
        ):]
        self.assertIn("const PsramString &lyricsText", lyrics_saver)
        self.assertIn("BasicJsonDocument<SpiRamAllocator>", lyrics_saver)
        self.assertNotIn("String cleanLyrics", lyrics_saver)

        fetch_all = WORKER[WORKER.index("case JOB_LYRICS_FETCH_ALL"):]
        fetch_all = fetch_all[:fetch_all.index(
            "case JOB_PERSIST_FAVORITE"
        )]
        self.assertIn("Storage.deleteTracklist(tl)", fetch_all)
        self.assertIn("delay(250)", fetch_all)

    def test_song_list_avoids_persistent_scroll_drift(self):
        list_style = UI[UI.index("static void style_list_row"):]
        list_style = list_style[:list_style.index("static lv_obj_t *create_panel_header")]
        self.assertNotIn(
            "lv_obj_add_style(row, &style_icon_button_pressed", list_style
        )
        self.assertIn("LV_STATE_PRESSED", list_style)

        song_list = UI[UI.index("lv_obj_t *container = lv_obj_create(tracklist_panel)"):]
        song_list = song_list[:song_list.index("lvgl_port_unlock();")]
        self.assertIn("LV_OBJ_FLAG_SCROLL_ELASTIC", song_list)
        self.assertIn("LV_OBJ_FLAG_SCROLL_MOMENTUM", song_list)
        self.assertIn("LV_OBJ_FLAG_SCROLL_CHAIN", song_list)
        self.assertIn("LV_EVENT_SCROLL_BEGIN", song_list)
        self.assertIn("lv_obj_clear_state", song_list)

        display_port = (ROOT / "Waveshare_ST7262_LVGL.h").read_text(
            encoding="utf-8", errors="replace"
        )
        self.assertIn("LVGL_PORT_DISP_WIDTH * 15", display_port)

    def test_live_diagnostics_overlay_is_optional_and_lightweight(self):
        globals_h = (ROOT / "AppGlobals.h").read_text(
            encoding="utf-8", errors="replace"
        )
        globals_cpp = (ROOT / "AppGlobals.cpp").read_text(
            encoding="utf-8", errors="replace"
        )
        display_cpp = (ROOT / "Waveshare_ST7262_LVGL.cpp").read_text(
            encoding="utf-8", errors="replace"
        )
        self.assertIn("setting_debug_overlay", globals_h)
        self.assertIn('getBool("debug_overlay", false)', globals_cpp)
        self.assertIn('putBool("debug_overlay", setting_debug_overlay)', globals_cpp)
        self.assertIn("display_monitor_callback", display_cpp)
        self.assertIn("disp_drv.monitor_cb = display_monitor_callback", display_cpp)

        overlay = UI[UI.index("static void refresh_debug_overlay"):]
        overlay = overlay[:overlay.index("// Add/Edit UI")]
        self.assertIn("heap_caps_get_largest_free_block", overlay)
        self.assertIn("ESP.getMinFreeHeap", overlay)
        self.assertIn("ESP.getFreePsram", overlay)
        self.assertIn("BackgroundWorker::getQueueSize", overlay)
        self.assertIn("lv_timer_create", overlay)
        self.assertIn("2000", overlay)
        self.assertIn("LV_OBJ_FLAG_CLICKABLE", overlay)

        settings = UI[UI.index("void show_settings_ui"):]
        self.assertIn('lv_label_set_text(lbl_debug_overlay, "Live overlay")', settings)
        self.assertIn("set_debug_overlay_enabled", settings)


if __name__ == "__main__":
    unittest.main()
