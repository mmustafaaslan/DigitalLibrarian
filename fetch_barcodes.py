
import json
import time
import urllib.request
import urllib.parse
import sys

def fetch_barcodes(total_needed=1000):
    collected_barcodes = set()
    offset = 0
    limit = 100 # Max usually allowed per page
    
    headers = {
        'User-Agent': 'DigitalLibrarian/1.0 ( mail@example.com )'
    }

    print(f"Starting fetch for {total_needed} barcodes...")

    while len(collected_barcodes) < total_needed:
        query = "tag:rock AND format:CD AND barcode:*"
        # Encode query
        params = urllib.parse.urlencode({
            'query': query,
            'limit': limit,
            'offset': offset,
            'fmt': 'json'
        })
        url = f"https://musicbrainz.org/ws/2/release/?{params}"
        
        try:
            req = urllib.request.Request(url, headers=headers)
            with urllib.request.urlopen(req) as response:
                if response.status != 200:
                    print(f"Error: {response.status}")
                    break
                
                data = json.loads(response.read().decode())
                releases = data.get('releases', [])
                
                if not releases:
                    print("No more releases found.")
                    break
                
                count_before = len(collected_barcodes)
                for r in releases:
                    bc = r.get('barcode', '')
                    # Simple validation: digits only, reasonable length
                    if bc and bc.isdigit() and len(bc) > 3:
                        collected_barcodes.add(bc)
                
                count_after = len(collected_barcodes)
                new_found = count_after - count_before
                print(f"Offset {offset}: Found {new_found} new barcodes. Total unique: {count_after}")
                
                offset += limit
                
                # Respect Rate Limit (1 req/sec)
                time.sleep(1.2)
                
        except Exception as e:
            print(f"Exception: {e}")
            time.sleep(2) # Wait a bit longer on error
            
        if offset > 2000 and len(collected_barcodes) < total_needed:
             # Safety break if we simply can't find enough unique ones deeply
             print("Giving up searching deeper.")
             break

    # Save to file
    out_file = "barcodes_1000.txt"
    with open(out_file, 'w', encoding='utf-8') as f:
        # Take exactly 1000 if we have more
        final_list = list(collected_barcodes)[:total_needed]
        f.write('\n'.join(final_list))
    
    print(f"Successfully wrote {len(final_list)} barcodes to {out_file}")

if __name__ == "__main__":
    fetch_barcodes()
