package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"log"
	"os"
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

// main wires CLI subcommands to the RemoteFS implementation so that users can
// run quick inspections without starting the daemon.
func main() {
	var (
		bucket    = flag.String("bucket", "", "S3 bucket name (required)")
		prefix    = flag.String("prefix", "", "virtual root prefix")
		region    = flag.String("region", "us-east-1", "S3 region")
		endpoint  = flag.String("endpoint", "", "optional S3-compatible endpoint")
		accessKey = flag.String("access-key", "", "S3 access key")
		secretKey = flag.String("secret-key", "", "S3 secret key")
		localRoot = flag.String("local-root", "/remote", "virtual local path that is considered remote backed")
		cacheDir  = flag.String("cache-dir", "", "directory for the on-disk cache (defaults to temp dir)")
		cacheSize = flag.Int64("cache-size", 512*1024*1024, "max cache size in bytes")
		timeout   = flag.Duration("timeout", 30*time.Second, "RPC timeout")
		socket    = flag.String("socket", "", "Unix socket path for the serve command")
		listen    = flag.String("listen", "", "TCP listen address for the serve command")
	)
	flag.Parse()
	if *bucket == "" {
		log.Fatal("bucket is required")
	}
	if flag.NArg() < 1 {
		log.Fatal("expected command: stat|ls|cat")
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

	switch flag.Arg(0) {
	case "stat":
		if flag.NArg() < 2 {
			log.Fatal("stat needs a path")
		}
		meta, err := fs.Stat(ctx, flag.Arg(1))
		if err != nil {
			log.Fatal(err)
		}
		fmt.Printf("%s\t%d bytes\t%s\tetag=%s\n", meta.Path, meta.Size, meta.LastModified.Format(time.RFC3339), meta.ETag)
	case "ls":
		target := ""
		if flag.NArg() > 1 {
			target = flag.Arg(1)
		}
		items, err := fs.ReadDir(ctx, target)
		if err != nil {
			log.Fatal(err)
		}
		for _, item := range items {
			if item.IsDir {
				fmt.Printf("[dir]\t%s\n", item.Path)
			} else {
				fmt.Printf("%d\t%s\n", item.Size, item.Path)
			}
		}
	case "cat":
		if flag.NArg() < 2 {
			log.Fatal("cat needs a path")
		}
		reader, err := fs.ReadFile(ctx, flag.Arg(1))
		if err != nil {
			log.Fatal(err)
		}
		defer reader.Close()
		if _, err := io.Copy(os.Stdout, reader); err != nil {
			log.Fatal(err)
		}
	case "serve":
		ipc, err := remotefs.NewIPCServer(fs)
		if err != nil {
			log.Fatalf("init IPC server: %v", err)
		}
		serveCtx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
		defer stop()
		if err := ipc.Serve(serveCtx, *socket, *listen); err != nil && err != context.Canceled {
			log.Fatalf("serve: %v", err)
		}
	default:
		log.Fatalf("unknown command %s", flag.Arg(0))
	}
}

// loadAWSConfig builds an AWS configuration that optionally overrides the
// endpoint/credentials for S3-compatible vendors.
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
