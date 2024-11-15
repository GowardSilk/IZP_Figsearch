import sys
import subprocess
from dataclasses import dataclass
import random
from time import time
import os

DEF_BMP_SIZE = (2560, 1440)

@dataclass
class BitmapSize:

    def __init__(self):
        self.width = DEF_BMP_SIZE[0]
        self.height = DEF_BMP_SIZE[1]

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
    def run_unit(exec: str) -> float:
        bmp: str = f"pics/bmp_{random.randint(0, 10000)}"
        generate_bmp(BitmapSize(), bmp)
        print([exec, "test", bmp])
        begin = time()
        ret = subprocess.run([exec, "test", bmp])
        end = time()
        if ret.returncode != 0:
            raise Exception(f"Failed test. {exec} returned: {ret.returncode}; expected: 0")
        print(f"Test took: {end - begin}ms")
        return end - begin

    match (cmd_type):
        case "time":
            print("[timed test]\n")
            average: float = 0
            n_tests: int = 100
            for _ in range(n_tests):
                average += run_unit(exec)
            average /= n_tests
            print(f"Average: {average}")

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
