package main

import (
	"context"
	"flag"
	"log"
	"os/signal"
	"syscall"
	"time"

	"example.com/s3rofs/pkg/objectstore"
	"example.com/s3rofs/pkg/remotefs"

	"github.com/aws/aws-sdk-go-v2/aws"
	"github.com/aws/aws-sdk-go-v2/config"
	"github.com/aws/aws-sdk-go-v2/credentials"
	"github.com/aws/aws-sdk-go-v2/service/s3"
)

// main launches the long-lived daemon that exposes the RemoteFS HTTP API over
// either a Unix socket or TCP loopback interface.
func main() {
	var (
		bucket    = flag.String("bucket", "", "S3 bucket name (required)")
		prefix    = flag.String("prefix", "", "virtual root prefix")
		region    = flag.String("region", "us-east-1", "S3 region")
		endpoint  = flag.String("endpoint", "", "optional S3-compatible endpoint")
		accessKey = flag.String("access-key", "", "S3 access key")
		secretKey = flag.String("secret-key", "", "S3 secret key")
		localRoot = flag.String("local-root", "/remote", "virtual local path exposed by the daemon")
		cacheDir  = flag.String("cache-dir", "", "directory for the on-disk cache (defaults to temp dir)")
		cacheSize = flag.Int64("cache-size", 512*1024*1024, "max cache size in bytes")
		timeout   = flag.Duration("timeout", 30*time.Second, "object store RPC timeout")
		socket    = flag.String("socket", "", "path to a Unix domain socket for IPC (takes precedence over listen)")
		listen    = flag.String("listen", "127.0.0.1:8484", "TCP listen address when -socket is empty")
	)
	flag.Parse()
	if *bucket == "" {
		log.Fatal("bucket is required")
	}

	ctx, cancel := context.WithTimeout(context.Background(), *timeout)
	defer cancel()

	awsCfg, err := loadAWSConfig(ctx, *region, *endpoint, *accessKey, *secretKey)
	if err != nil {
		log.Fatalf("load AWS config: %v", err)
	}
	client := s3.NewFromConfig(awsCfg)
	store := objectstore.NewS3Store(client, *bucket, *prefix)
	fs, err := remotefs.New(store, remotefs.Config{
		LocalRoot: *localRoot,
		CacheDir:  *cacheDir,
		CacheSize: *cacheSize,
	})
	if err != nil {
		log.Fatalf("init RemoteFS: %v", err)
	}
	warmCtx, warmCancel := context.WithTimeout(context.Background(), *timeout)
	defer warmCancel()
	if err := fs.WarmMetadataCache(warmCtx); err != nil {
		log.Fatalf("prime metadata cache: %v", err)
	}

	ipc, err := remotefs.NewIPCServer(fs)
	if err != nil {
		log.Fatalf("init IPC server: %v", err)
	}

	runCtx, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT)
	defer stop()

	if err := ipc.Serve(runCtx, *socket, *listen); err != nil && err != context.Canceled {
		log.Fatalf("serve: %v", err)
	}
}

// loadAWSConfig mirrors the CLI helper so the daemon can talk to vanilla S3 or
// compatible vendors.
func loadAWSConfig(ctx context.Context, region, endpoint, accessKey, secretKey string) (aws.Config, error) {
	loaders := []func(*config.LoadOptions) error{
		config.WithRegion(region),
	}
	if endpoint != "" {
		custom := aws.EndpointResolverWithOptionsFunc(func(service, region string, options ...interface{}) (aws.Endpoint, error) {
			return aws.Endpoint{
				URL:           endpoint,
				SigningRegion: region,
			}, nil
		})
		loaders = append(loaders, config.WithEndpointResolverWithOptions(custom))
	}
	if accessKey != "" && secretKey != "" {
		loaders = append(loaders, config.WithCredentialsProvider(credentials.NewStaticCredentialsProvider(accessKey, secretKey, "")))
	}
	return config.LoadDefaultConfig(ctx, loaders...)
}
