import sys
import subprocess
from dataclasses import dataclass
import random
from time import time
import os

@dataclass
class BitmapSize:
    width: int
    height: int

def generate_pix() -> str:
    return "1" if random.random() > 0.5 else "0"

def generate_bmp(size: BitmapSize, loc: str):
    with open(loc, "w+") as file:
        # write size
        file.write(str(size.height) + " " + str(size.width) + "\n")
        # write buffer
        file.writelines(' '.join(generate_pix() for _ in range(size.width)) + "\n" for _ in range(size.height))

def cmd_test(cmd_type: str, exec: str) -> None:
    def run_unit(exec: str):
        bmp: str = f"pics/bmp_{random.randint(0, 10000)}"
        generate_bmp(BitmapSize(1920, 1080), bmp)
        print([exec, "test", bmp])
        ret = subprocess.run([exec, "test", bmp])
        if ret.returncode != 0:
            raise Exception(f"Failed test. {exec} returned: {ret.returncode}; expected: 0")

    match (cmd_type):
        case "time":
            print("[timed test]\n")
            for _ in range(10):
                begin = time()
                run_unit(exec)
                print(f"Test took: {time() - begin}ms")

        case "functionality":
            print("[functionality test]\n")
        case _:
            assert False

    return None

def prepare() -> None:
    os.makedirs("pics", exist_ok=True)
    return None

if __name__ == "__main__":
    assert len(sys.argv) > 2

    if len(sys.argv) == 3:
        prepare()
        cmd_test(sys.argv[1], sys.argv[2])
    elif len(sys.argv) == 2:
        prepare()
        cmd_test("functionality", sys.argv[2])
    else:
        raise Exception("Invalid number of arguments!")
