{-# LANGUAGE ImportQualifiedPost #-}
{-# LANGUAGE OverloadedStrings #-}

module Auth (register, unregister, checkAuth, TokenStore, initTokenStore, verifyAdmin, AuthStatus (..)) where

import Control.Concurrent.STM (STM, modifyTVar, newTVarIO)
import Control.Concurrent.STM.TVar (TVar, readTVar)
import Control.Exception (Exception, throwIO)
import Control.Monad.STM (atomically)
import Crypto.Scrypt (EncryptedPass (..), Pass (..), verifyPass')
import Data.ByteString.Char8 (pack)
import Data.Functor ((<&>))
import Data.Map.Strict qualified as Map
import Data.Text (Text)
import Data.Text.Encoding (encodeUtf8)
import Data.Text.Lazy qualified as L
import Data.UUID (UUID, fromText)
import Data.UUID.V4 (nextRandom)
import Network.Socket (SockAddr)

data AuthStatus = BadToken | Allowed | Disallowed

data BadPassfileException = WrongSpec | CantDecode deriving (Show)

data RequestException = BadTokenException deriving (Show)

instance Exception BadPassfileException

instance Exception RequestException

type Token = UUID

--  A named resource. Likely a pane.
type Name = Text

type TokenStore = TVar TokenStoreInner

data TokenStoreInner = TokenStoreInner
  { adminCred :: EncryptedPass,
    by_token :: Map.Map Name Token,
    by_ip :: Map.Map SockAddr Name
  }

initTokenStore :: IO TokenStore
initTokenStore =
  readFile "admin.pass" >>= getCreds >>= \cred ->
    newTVarIO $
      TokenStoreInner
        { adminCred = EncryptedPass {getEncryptedPass = cred},
          by_token = Map.empty,
          by_ip = Map.empty
        }
  where
    getCreds file = parse $ lines file
    parse [cred] = return $ pack cred
    parse _ = throwIO WrongSpec

checkAuth :: TokenStore -> Name -> L.Text -> IO AuthStatus
checkAuth ts name t = case fromText $ L.toStrict t of
  Just token -> atomically $ checkAuth' token
  Nothing -> return BadToken
  where
    checkAuth' uuid = check uuid . by_token <$> readTVar ts
    check uuid toks = case toks Map.!? name of
      Just x -> if x == uuid then Allowed else Disallowed
      Nothing -> Disallowed

unregister :: TokenStore -> Name -> IO ()
unregister ts name = atomically $ modifyTVar ts unregister'
  where
    unregister' (TokenStoreInner {adminCred = admin, by_token = toks, by_ip = ips}) =
      let toks' = Map.delete name toks
       in TokenStoreInner
            { adminCred = admin,
              by_token = toks',
              -- TODO: Remove all references to `name` in IP. This would require a
              -- properly-done multi-indexed map type that I am *not* writing one
              -- day before the event.
              by_ip = ips
            }

register :: TokenStore -> SockAddr -> Name -> IO (Maybe Token)
register ts peer name = nextRandom >>= atomically . register'
  where
    register' uuid = readTVar ts >>= tryUpdate uuid
    tryUpdate :: Token -> TokenStoreInner -> STM (Maybe Token)
    tryUpdate tok ts' = case by_token ts' Map.!? name of
      Just _ -> return Nothing
      Nothing -> modifyTVar ts (withTok tok) >> return (Just tok)

    withTok tok tsi =
      TokenStoreInner
        { adminCred = adminCred tsi,
          by_token = Map.insert name tok $ by_token tsi,
          by_ip = Map.insert peer name $ by_ip tsi
        }

verifyAdmin :: TokenStore -> Text -> STM Bool
verifyAdmin ts tok = readTVar ts <&> verify
  where
    verify ts' = verifyPass' (Pass {getPass = encodeUtf8 tok}) (adminCred ts')
