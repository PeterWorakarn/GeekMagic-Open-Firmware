function locketHandler() {
  return {
    loading: false,
    url: "",
    intervalMin: 5,
    lastStatus: "",
    lastOk: false,
    message: "",

    fetchConfig() {
      apiFetch("/api/v1/locket/config")
        .then((r) => r.json())
        .then((data) => {
          this.url = data.url || "";
          this.intervalMin = data.interval_min || 5;
          this.lastStatus = data.lastStatus || "";
          this.lastOk = data.lastOk || false;
        })
        .catch((err) => {
          this.lastStatus = "error fetching config";
          console.error(err);
        });
    },

    save() {
      if (!this.url) {
        this.message = "URL is required";
        this.lastOk = false;
        return;
      }
      this.loading = true;
      this.message = "";
      const payload = {
        url: this.url,
        interval_min: Number(this.intervalMin) || 5,
      };
      apiFetch("/api/v1/locket/config", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
      })
        .then((r) => r.json())
        .then((data) => {
          if (data.status === "ok") {
            this.lastStatus = data.lastStatus || "";
            this.lastOk = !!data.fetched;
            this.message = data.fetched
              ? "Saved & image displayed"
              : "Saved, but fetch failed: " + (data.lastStatus || "");
          } else {
            this.message = data.message || "save failed";
            this.lastOk = false;
          }
        })
        .catch((err) => {
          this.message = "save failed";
          this.lastOk = false;
          console.error(err);
        })
        .finally(() => {
          this.loading = false;
        });
    },

    init() {
      this.fetchConfig();
    },
  };
}
