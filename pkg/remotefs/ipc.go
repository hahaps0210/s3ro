package remotefs

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/user"
	"path/filepath"
	"strconv"
	"time"

	"example.com/s3rofs/pkg/objectstore"
)

const (
	modeDirBits = 0o040000
	modeRegBits = 0o100000
	dirPerms    = 0o550
	filePerms   = 0o440
)

// POSIXEntry mirrors the metadata callers expect from stat/readdir.
type POSIXEntry struct {
	Path         string    `json:"Path"`
	Size         int64     `json:"Size"`
	ETag         string    `json:"ETag"`
	LastModified time.Time `json:"LastModified"`
	IsDir        bool      `json:"IsDir"`
	Mode         uint32    `json:"Mode"`
	UID          int       `json:"UID"`
	GID          int       `json:"GID"`
	User         string    `json:"User"`
	Group        string    `json:"Group"`
}

// IPCServer exposes RemoteFS through HTTP/IPC so other languages can consume it.
type IPCServer struct {
	fs    *FileSystem
	uid   int
	gid   int
	user  string
	group string
}

// NewIPCServer constructs a server bound to the provided filesystem.
func NewIPCServer(fs *FileSystem) (*IPCServer, error) {
	if fs == nil {
		return nil, fmt.Errorf("filesystem is required")
	}
	s := &IPCServer{
		fs:  fs,
		uid: os.Geteuid(),
		gid: os.Getegid(),
	}
	if u, err := user.LookupId(strconv.Itoa(s.uid)); err == nil {
		s.user = u.Username
	}
	if g, err := user.LookupGroupId(strconv.Itoa(s.gid)); err == nil {
		s.group = g.Name
	}
	return s, nil
}

// Handler returns an http.Handler exposing /stat, /ls, and /cat endpoints.
func (s *IPCServer) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/stat", s.handleStat)
	mux.HandleFunc("/ls", s.handleList)
	mux.HandleFunc("/cat", s.handleCat)
	return mux
}

// Serve listens on the provided socket or TCP address until ctx is cancelled.
func (s *IPCServer) Serve(ctx context.Context, socketPath, listenAddr string) error {
	if socketPath == "" && listenAddr == "" {
		listenAddr = "127.0.0.1:8080"
	}
	l, err := createListener(socketPath, listenAddr)
	if err != nil {
		return err
	}
	defer l.Close()

	server := &http.Server{Handler: s.Handler()}
	errCh := make(chan error, 1)
	go func() {
		if serveErr := server.Serve(l); serveErr != nil && serveErr != http.ErrServerClosed {
			errCh <- serveErr
		}
		close(errCh)
	}()

	select {
	case <-ctx.Done():
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = server.Shutdown(shutdownCtx)
		return ctx.Err()
	case serveErr := <-errCh:
		return serveErr
	}
}

func (s *IPCServer) handleStat(w http.ResponseWriter, r *http.Request) {
	path := r.URL.Query().Get("path")
	if path == "" {
		path = s.fs.LocalRoot()
	}
	meta, err := s.fs.Stat(r.Context(), path)
	if err != nil {
		writeErrorFor(w, err)
		return
	}
	writeJSON(w, s.entryFromMeta(meta))
}

func (s *IPCServer) handleList(w http.ResponseWriter, r *http.Request) {
	path := r.URL.Query().Get("path")
	if path == "" {
		path = s.fs.LocalRoot()
	}
	items, err := s.fs.ReadDir(r.Context(), path)
	if err != nil {
		writeErrorFor(w, err)
		return
	}
	out := make([]POSIXEntry, 0, len(items))
	for _, item := range items {
		out = append(out, s.entryFromMeta(item))
	}
	writeJSON(w, out)
}

func (s *IPCServer) handleCat(w http.ResponseWriter, r *http.Request) {
	path := r.URL.Query().Get("path")
	if path == "" {
		writeHTTPError(w, http.StatusBadRequest, "path query parameter is required")
		return
	}
	reader, err := s.fs.ReadFile(r.Context(), path)
	if err != nil {
		writeErrorFor(w, err)
		return
	}
	defer reader.Close()
	w.Header().Set("Content-Type", "application/octet-stream")
	_, _ = io.Copy(w, reader)
}

func (s *IPCServer) entryFromMeta(meta objectstore.FileMeta) POSIXEntry {
	entry := POSIXEntry{
		Path:         meta.Path,
		Size:         meta.Size,
		ETag:         meta.ETag,
		LastModified: meta.LastModified,
		IsDir:        meta.IsDir,
		UID:          s.uid,
		GID:          s.gid,
		User:         s.user,
		Group:        s.group,
	}
	if entry.LastModified.IsZero() {
		entry.LastModified = time.Now()
	}
	entry.Mode = defaultMode(entry.IsDir)
	return entry
}

func defaultMode(isDir bool) uint32 {
	if isDir {
		return uint32(modeDirBits | dirPerms)
	}
	return uint32(modeRegBits | filePerms)
}

func createListener(socketPath, listenAddr string) (net.Listener, error) {
	if socketPath != "" {
		if err := os.MkdirAll(filepath.Dir(socketPath), 0o755); err != nil {
			return nil, fmt.Errorf("prepare socket dir: %w", err)
		}
		if err := os.RemoveAll(socketPath); err != nil {
			return nil, fmt.Errorf("remove stale socket: %w", err)
		}
		l, err := net.Listen("unix", socketPath)
		if err != nil {
			return nil, fmt.Errorf("unix listen: %w", err)
		}
		return l, nil
	}
	l, err := net.Listen("tcp", listenAddr)
	if err != nil {
		return nil, fmt.Errorf("tcp listen: %w", err)
	}
	return l, nil
}

func writeJSON(w http.ResponseWriter, payload interface{}) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(payload)
}

func writeHTTPError(w http.ResponseWriter, status int, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": msg})
}

func writeErrorFor(w http.ResponseWriter, err error) {
	status := http.StatusInternalServerError
	if IsNotFound(err) {
		status = http.StatusNotFound
	}
	writeHTTPError(w, status, err.Error())
}
